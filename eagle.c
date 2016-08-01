/*
EAGLE: explicit alternative genome likelihood evaluator
Given the sequencing data and candidate variant, explicitly test 
the alternative hypothesis against the reference hypothesis

Copyright 2016 Tony Kuo
This program is distributed under the terms of the GNU General Public License
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <pthread.h>
#include "eagle.h"
#include "htslib/sam.h"
#include "htslib/faidx.h"
#include "htslib/khash.h"

/* Constants */
#define ALPHA 1.3    // Factor to account for longer read lengths lowering the probability a sequence matching an outside paralogous source
#define OMEGA 1.0e-4 // Prior probability of read originating from an outside paralogous source
#define REFPRIOR log(0.5)

/* Precalculated log values */
#define M_1_LOG10E 1.0/M_LOG10E
#define M_1_LN10 1.0/M_LN10
#define LG3 log(3.0)
#define LG50 log(0.5)
#define LG10 log(0.1)
#define LG90 log(0.9)
#define LGALPHA log(ALPHA)
#define LGOMEGA log(OMEGA)
#define LG1_OMEGA log(1.0-OMEGA)

/* Command line arguments */
static int numproc;
static int distlim;
static int maxh;
static int mvh;
static double hetbias;
static int pao;
static int debug;

/* Time info */
static time_t now; 
static struct tm *time_info; 
#define print_status(M, ...) time(&now); time_info = localtime(&now); fprintf(stderr, M, ##__VA_ARGS__);

/* Mapping tables */
static int seqnt_map[26];
static int compl_map[26];

KHASH_MAP_INIT_STR(rsh, Vector)   // hashmap: string key, vector value
static khash_t(rsh) *refseq_hash; // pointer to hashmap
static pthread_mutex_t refseq_lock; 

/*
char *strdup(const char *src) {
    size_t n = strlen(src) + 1;
    char *des = malloc(n * sizeof *des);
    return des ? memcpy(des, src, n) : NULL;
}
*/

static inline void str_resize(char **str, size_t size) {
    char *p = realloc(*str, size * sizeof *str);
    if (p == NULL) { exit_err("failed to realloc in str_resize\n"); }
    else { *str = p; }
}

static inline int parse_num(const char *str) {
    errno = 0;
    char *end;
    int num = strtol(str, &end, 0);
    if (end != str && *end != '\0') { exit_err("failed to convert '%s' to int with leftover string '%s'\n", str, end); }
    return num;
}

static inline double sum(const double *a, int size) {
    double s = 0;
    while (--size >= 0) s += a[size];
    return s;
}

static inline double log_add_exp(double a, double b) {
    double max_exp = a > b ? a : b;
    return log(exp(a - max_exp) + exp(b - max_exp)) + max_exp;
}

static inline double log_sum_exp(const double *a, int size) {
    int i;
    double max_exp = a[0]; 
    for (i = 1; i < size; ++i) { 
        if (a[i] > max_exp) max_exp = a[i]; 
    }
    double s = 0;
    for (i = 0; i < size; ++i) s += exp(a[i] - max_exp);
    return log(s) + max_exp;
}

static inline double *reverse(double *a, int size) {
    int i = 0;
    double *b = malloc(size * sizeof *b);
    while (--size >= 0) b[i++] = a[size];
    return b;
}

static inline char *reverse_compl(char *a, int size) {
    char *b = malloc((size + 1) * sizeof *b);
    b[size] = '\0';
    int i = 0;
    while (--size >= 0) b[i++] = compl_map[a[size] - 'A'];
    return b;
}

static inline void set_prob_matrix(double *matrix, const char *seq, int read_length, const double *is_match, const double *no_match) {
    int i, b; // array[width * row + col] = value
    for (b = 0; b < read_length; ++b) {
        for (i = 0; i < 5; ++i) matrix[5 * b + i] = no_match[b];
        matrix[5 * b + seqnt_map[seq[b] - 'A']] = is_match[b];
    }
}

static inline double calc_prob(const double *matrix, int read_length, const char *seq, int seq_length, int pos, double baseline) {
    int b; // array[width * row + col] = value
    int n = pos + read_length;
    double probability = 0;
    for (b = pos;  b < n; ++b) {
        if (b < 0) continue;
        if (b >= seq_length) break;
        probability += matrix[5 * (b - pos) + seqnt_map[seq[b] - 'A']]; 
        if (probability < baseline - 10) break; // stop if less than 1% contribution to baseline (best/highest) probability mass
    }
    return probability;
}

static inline double calc_prob_distrib(const double *matrix, int read_length, const char *seq, int seq_length, int pos) {
    int i;
    int n1 = pos - read_length;
    int n2 = pos + read_length;
    double probability = 0;
    double baseline = calc_prob(matrix, read_length, seq, seq_length, pos, -1000); // first probability at given pos, likely the highest, for initial baseline
    for (i = n1; i < n2; ++i) {
        if (i + read_length < 0) continue;
        if (i >= seq_length) break;
        probability = probability == 0 ? calc_prob(matrix, read_length, seq, seq_length, i, baseline) : log_add_exp(probability, calc_prob(matrix, read_length, seq, seq_length, i, baseline));
        if (probability > baseline) baseline = probability;
    }
    return probability;
}

static void combinations(char **output, int k, int n, int *ncombos) {
    int i, c[k];
    for (i = 0; i < k; ++i) c[i] = i; // first combination
    while (1) { // while (next_comb(c, k, n)) {
        char token[k + 2];
        for (i = 0; i < k; ++i) token[i] = c[i] + '\t' + 1; // record the combination as ('\t'+1)-indexed
        token[k] = '\t';
        token[k + 1] = '\0';
        str_resize(output, strlen(*output) + strlen(token) + 1);
        strcat(*output, token);
        ++(*ncombos);

        i = k - 1;
        ++c[i];
        while ((i >= 0 && i < k) && (c[i] >= n - k + 1 + i)) {
            --i;
            ++c[i];
        }
        /* Combination (n-k, n-k+1, ..., n) reached. No more combinations can be generated */
        if (c[0] > n - k) break; // return 0;
        /* c now looks like (..., x, n, n, n, ..., n), turn it into (..., x, x + 1, x + 2, ...) */
        for (i = i + 1; i < k; ++i) c[i] = c[i - 1] + 1;
        // return 1;
    }
}

static char *powerset(int n, int *ncombos) {
    char *combo = malloc(sizeof *combo);
    combo[0] = '\0';

    combinations(&combo, 1, n, ncombos); // k = 1
    if (n > 1) {
        combinations(&combo, n, n, ncombos); // k = n
        int k;
        for (k = 2; k <= n - 1; ++k) { // k = 2 : n - 1
            combinations(&combo, k, n, ncombos);
            if (*ncombos - n - 1 >= maxh) break;
        }
    }
    return combo;
}

static int nat_sort_cmp(const void *a, const void *b, enum type var_type) {
    char *str1, *str2;
    switch (var_type) {
        case VARIANT_T: {
            Variant *c1 = *(Variant **)a;
            Variant *c2 = *(Variant **)b;
            if (strcasecmp(c1->chr, c2->chr) == 0) return (c1->pos > c2->pos) - (c1->pos < c2->pos);
            str1 = strdup(c1->chr);
            str2 = strdup(c2->chr);
            c1 = NULL;
            c2 = NULL;
            break;
        }
        default:
            str1 = strdup(*(char **)a);
            str2 = strdup(*(char **)b);
            break;
    }
    char *s1 = str1;
    char *s2 = str2;
    int cmp = 0;
    while (cmp == 0 && *s1 != '\0' && *s2 != '\0') {
        if (isspace(*s1) && isspace(*s2)) { // ignore whitespace
            s1 += 1;
            s2 += 1;
        }
        else if ((isalpha(*s1) && isalpha(*s2)) || (ispunct(*s1) && ispunct(*s2))) { // compare alphabet and punctuation
            *s1 = tolower(*s1);
            *s2 = tolower(*s2);
            cmp = (*s1 > *s2) - (*s1 < *s2);
            s1 += 1;
            s2 += 1;
        }
        else { // compare digits
            int i1, i2, n1, n2, t1, t2;
            t1 = sscanf(s1, "%d%n", &i1, &n1);
            if (t1 == 0) t1 = sscanf(s1, "%*[^0123456789]%d%n", &i1, &n1);
            t2 = sscanf(s2, "%d%n", &i2, &n2);
            if (t2 == 0) t2 = sscanf(s2, "%*[^0123456789]%d%n", &i2, &n2);

            if (t1 < 1 || t2 < 1) { // one string has no digits
                cmp = strcmp(s1, s2);
            }
            else {
                cmp = (i1 > i2) - (i1 < i2);
                if (cmp == 0) { // first set of digits are equal, check further
                    s1 += n1;
                    s2 += n2;
                }
            }
        }
    }
    free(str1); str1 = NULL;
    free(str2); str2 = NULL;
    return cmp;
}

static int nat_sort_str(const void *a, const void *b) {
    return nat_sort_cmp(a, b, VOID_T);
}

static int nat_sort_var(const void *a, const void *b) {
    return nat_sort_cmp(a, b, VARIANT_T);
}

void destroy_variant(Variant *v) {
    if (v == NULL) return;
    v->pos = 0;
    free(v->chr); v->chr = NULL;      
    free(v->ref); v->ref = NULL;
    free(v->alt); v->alt = NULL;
}

void destroy_read(Read *r) {
    if (r == NULL) return;
    r->tid = r->pos = r->length = r->n_cigar = r->inferred_length = 0;
    free(r->name); r->name = NULL;
    free(r->chr); r->chr = NULL;
    free(r->qseq); r->qseq = NULL;
    free(r->qual); r->qual = NULL;
    free(r->flag); r->flag = NULL;
    free(r->cigar_opchr); r->cigar_opchr = NULL;
    free(r->cigar_oplen); r->cigar_oplen = NULL;
    free(r->multimap); r->multimap = NULL;
}

void destroy_fasta(Fasta *f) {
    if (f == NULL) return;
    f->seq_length = 0;
    free(f->seq); f->seq = NULL;      
    free(f->name); f->name = NULL;
}

void vector_init(Vector *a, size_t initial_size, enum type var_type) {
    a->size = 0;
    a->type = var_type;
    a->capacity = initial_size;
    a->data = malloc(initial_size * sizeof (void *));
}

void vector_add(Vector *a, void *entry) {
    if (a->size >= a->capacity) {
        a->capacity *= 2;
        void **p = realloc(a->data, a->capacity * sizeof (void *));
        if (p == NULL) { exit_err("failed to realloc in vector_add\n"); }
        else { a->data = p; }
    }
    a->data[a->size++] = entry;
}

void vector_del(Vector *a, int i) {
    a->data[i] = NULL;
    if (i == --a->size) return;

    void **p = malloc(a->capacity * sizeof (void *));
    if (i == 0 && a->size > 0) {
        memcpy(&(p[0]), &(a->data[1]), a->size * sizeof (void *));
    }
    else {
        memcpy(&(p[0]), &(a->data[0]), i * sizeof (void *));
        memcpy(&(p[i]), &(a->data[i + 1]), (a->size - i) * sizeof *p);
    }
    free(a->data); a->data = NULL;
    a->data = p;
}

void *vector_pop(Vector *a) {
    if (a->size <= 0) return NULL;
    void *entry = a->data[--a->size];
    a->data[a->size] = NULL;
    return entry;
}

Vector *vector_create(size_t initial_size, enum type var_type) {
    Vector *a = malloc(sizeof (Vector));
    vector_init(a, initial_size, var_type);
    return a;
}

Vector *vector_dup(Vector *a) {
    Vector *v = vector_create(a->capacity, a->type);
    v->size = a->size;
    memcpy(&(v->data[0]), &(a->data[0]), a->size * sizeof (void *));
    return v;
}

void vector_free(Vector *a) {
    a->size = 0;
    free(a->data); a->data = NULL;
}

void vector_destroy(Vector *a) {
    size_t i;
    enum type var_type = a->type;
    for (i = 0; i < a->size; ++i) {
        switch (var_type) {
            case VARIANT_T:
                destroy_variant((Variant *)a->data[i]);
                break;
            case READ_T:
                destroy_read((Read *)a->data[i]);
                break;
            case FASTA_T:
                destroy_fasta((Fasta *)a->data[i]);
                break;
            default:
                break;
        }
        free(a->data[i]); a->data[i] = NULL;
    }
    a->size = a->capacity = 0;
    free(a->data); a->data = NULL;
}

Vector *read_vcf(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) { exit_err("failed to open VCF file %s\n", filename); }

    Vector *var_vector = vector_create(64, VARIANT_T);

    char *line = NULL;
    ssize_t read_file = 0;
    size_t line_length = 0;
    while ((read_file = getline(&line, &line_length, file)) != -1) {
        if (line_length <= 0 || line[strspn(line, " \t\v\r\n")] == '\0') continue; // blank line
        if (line[0] == '#') continue;

        int pos;
        char chr[line_length], ref[line_length], alt[line_length];
        int t = sscanf(line, "%s %d %*[^\t ] %s %s", chr, &pos, ref, alt);
        if (t < 4) { exit_err("bad fields in VCF file %s\n", filename); }

        int n1, n2;
        char *s1, *s2, ref_token[strlen(ref) + 1], alt_token[strlen(alt) + 1];
        for (s1 = ref; sscanf(s1, "%[^, ]%n", ref_token, &n1) == 1 || sscanf(s1, "%[-]%n", ref_token, &n1) == 1; s1 += n1 + 1) { // heterogenenous non-reference (comma-delimited) as separate entries
            for (s2 = alt; sscanf(s2, "%[^, ]%n", alt_token, &n2) == 1 || sscanf(s2, "%[-]%n", alt_token, &n2) == 1; s2 += n2 + 1) {
                Variant *v = malloc(sizeof (Variant));
                v->chr = strdup(chr);
                v->pos = pos;
                v->ref = strdup(ref_token);
                v->alt = strdup(alt_token);
                vector_add(var_vector, v);
                if (*(s2 + n2) != ',') break;
            }
            if (*(s1 + n1) != ',') break;
        }
    }
    free(line); line = NULL;
    fclose(file);
    qsort(var_vector->data, var_vector->size, sizeof (void *), nat_sort_var);
    print_status("Read VCF: %s\t%i entries\t%s", filename, (int)var_vector->size, asctime(time_info));
    return var_vector;
}

void read_fasta(const char *fa_file) {
    faidx_t *fai = fai_load(fa_file);
    if (fai == NULL) { exit_err("failed to open FA index %s\n", fa_file); }

    char *filename = malloc((strlen(fa_file) + 5) * sizeof *filename);
    filename[0] = '\0';
    strcat(filename, fa_file);
    strcat(filename, ".fai");
    FILE *file = fopen(filename, "r");
    if (file == NULL) { exit_err("failed to open FA index for parsing %s\n", filename); }
    free(filename); filename = NULL;

    char *line = NULL;
    ssize_t read_file = 0;
    size_t line_length = 0;
    while ((read_file = getline(&line, &line_length, file)) != -1) {
        if (line_length <= 0 || line[strspn(line, " \t\v\r\n")] == '\0') continue; // blank line
        if (line[0] == '#') continue;

        char name[line_length];
        int t = sscanf(line, "%s%*[^ \t\v\r\n]", name);
        if (t < 1) { exit_err("bad fields in FA index file\n"); }
        if (!faidx_has_seq(fai, name)) { exit_err("failed to find sequence %s in reference %s\n", name, fa_file); }

        Fasta *f = malloc(sizeof (Fasta));
        f->name = strdup(name);
        f->seq = fai_fetch(fai, name, &f->seq_length);
        char *s;
        for (s = f->seq; *s != '\0'; ++s) *s = toupper(*s);

        int absent;
        khiter_t k = kh_put(rsh, refseq_hash, f->name, &absent);
        Vector *node = &kh_val(refseq_hash, k); // point to the bucket associated to k
        if(absent) vector_init(node, 8, FASTA_T);
        vector_add(node, f);
    }
    free(line); line = NULL;
    fclose(file);
    fai_destroy(fai);
    print_status("Read reference genome: %s\t%s", fa_file, asctime(time_info));
}

static Vector *fetch_reads(const char *bam_file, const char *region) {
    samFile *sam_in = sam_open(bam_file, "r"); // open bam file
    if (sam_in == NULL) { exit_err("failed to open BAM file %s\n", bam_file); }
    bam_hdr_t *bam_header = sam_hdr_read(sam_in); // bam header
    if (bam_header == 0) { exit_err("bad header %s\n", bam_file); }
    hts_idx_t *bam_idx = sam_index_load(sam_in, bam_file); // bam index
    if (bam_idx == NULL) { exit_err("failed to open BAM index %s\n", bam_file); }

    Vector *read_vector = vector_create(64, READ_T);
    hts_itr_t *iter = sam_itr_querys(bam_idx, bam_header, region); // read iterator
    if (iter != NULL) {
        bam1_t *aln = bam_init1(); // initialize an alignment
        while (sam_itr_next(sam_in, iter, aln) >= 0) {
            size_t i;
            Read *read = malloc(sizeof (Read));
            read->name = strdup((char *)aln->data);
            read->tid = aln->core.tid;
            read->chr = strdup(bam_header->target_name[read->tid]);
            read->pos = aln->core.pos;
            read->length = aln->core.l_qseq;

            read->qseq = malloc((read->length + 1) * sizeof read->qseq);
            read->qual = malloc(read->length * sizeof read->qual);
            uint8_t *qual = bam_get_qual(aln);
            for (i = 0; i < read->length; ++i) {
                read->qseq[i] = toupper(seq_nt16_str[bam_seqi(bam_get_seq(aln),i)]); // get nucleotide id and convert into IUPAC id.
                read->qual[i] = (double)qual[i] / -10;
            }
            read->qseq[read->length] = '\0';

            uint32_t *cigar = bam_get_cigar(aln);
            read->n_cigar = aln->core.n_cigar;
            read->cigar_oplen = malloc(read->n_cigar * sizeof read->cigar_oplen);
            read->cigar_opchr = malloc((read->n_cigar + 1) * sizeof read->cigar_opchr);
            for (i = 0; i < read->n_cigar; ++i) {
                read->cigar_oplen[i] = bam_cigar_oplen(cigar[i]);
                read->cigar_opchr[i] = bam_cigar_opchr(cigar[i]);
            }
            read->cigar_opchr[read->n_cigar] = '\0';
            read->inferred_length = bam_cigar2qlen(read->n_cigar, cigar);

            char *str;
            read->flag = NULL;
            str = bam_flag2str(aln->core.flag);
            if (str != NULL) read->flag = strdup(str);
            free(str); str = NULL;

            read->multimap = NULL;
            str = (char *)bam_aux_get(aln, "XA");
            if (str != NULL) read->multimap = strdup(str);

            vector_add(read_vector, read);
        }
        bam_destroy1(aln);
    }
    hts_itr_destroy(iter);
    hts_idx_destroy(bam_idx);
    bam_hdr_destroy(bam_header);
    sam_close(sam_in);
    return read_vector;
}

static Fasta *fetch_refseq(const char *name, const char *fa_file) {
    pthread_mutex_lock(&refseq_lock);
    size_t i;
	khiter_t k = kh_get(rsh, refseq_hash, name);
    if (k != kh_end(refseq_hash)) {
        Vector *node = &kh_val(refseq_hash, k); // point to the bucket associated to k
        Fasta **f = (Fasta **)node->data;
        for (i = 0; i < node->size; ++i) {
            if (strcmp(f[i]->name, name) == 0) {
                pthread_mutex_unlock(&refseq_lock);
                return f[i];
            }
        }
        exit_err("failed to find sequence %s in hash key %d\n", name, k);
    }

    faidx_t *fai = fai_load(fa_file);
    if (fai == NULL) { exit_err("failed to open FA index %s\n", fa_file); }
    if (!faidx_has_seq(fai, name)) { exit_err("failed to find sequence %s in reference %s\n", name, fa_file); }

    Fasta *f = malloc(sizeof (Fasta));
    f->name = strdup(name);
    f->seq = fai_fetch(fai, name, &f->seq_length);
    char *s;
    for (s = f->seq; *s != '\0'; ++s) *s = toupper(*s);

    int absent;
    k = kh_put(rsh, refseq_hash, f->name, &absent);
    Vector *node = &kh_val(refseq_hash, k); // point to the bucket associated to k
    if(absent) vector_init(node, 8, FASTA_T);
    vector_add(node, f);
    fai_destroy(fai);
    pthread_mutex_unlock(&refseq_lock);
    return f;
}

static char *construct_altseq(const char *refseq, int refseq_length, const Vector *var_combo, int *altseq_length) {
    size_t i, j;
    int offset = 0;
    char *altseq = strdup(refseq);
    *altseq_length = refseq_length;
    for (i = 0; i < var_combo->size; ++i) {
        Variant *curr = (Variant *)var_combo->data[i];
        size_t pos = curr->pos - 1 + offset;
        char *var_ref, *var_alt;
        if (curr->ref[0] == '-') { // account for "-" variant representations
            ++pos;
            var_ref = strdup("");
            var_alt = strdup(curr->alt);
        }
        else if (curr->alt[0] == '-') { // account for "-" variant representations
            var_ref = strdup(curr->ref);
            var_alt = strdup("");
        }
        else {
            var_ref = strdup(curr->ref);
            var_alt = strdup(curr->alt);
        }
        size_t var_ref_length = strlen(var_ref);
        size_t var_alt_length = strlen(var_alt);
        int delta = var_alt_length - var_ref_length;
        offset += delta;
        if (delta == 0) { // snps, equal length haplotypes
            for (j = 0; j < var_alt_length; ++j) altseq[j+pos] = var_alt[j];
        }
        else { // indels
            char *newalt = malloc((*altseq_length + delta + 1) * sizeof *newalt);
            memcpy(newalt, altseq, pos * sizeof *newalt);
            memcpy(newalt + pos, var_alt, var_alt_length * sizeof *newalt);
            memcpy(newalt + pos + var_alt_length, altseq + pos + var_ref_length, (*altseq_length - pos - var_ref_length) * sizeof *newalt);
            *altseq_length += delta;
            newalt[*altseq_length] = '\0';
            free(altseq); altseq = NULL;
            altseq = newalt;
        }
        free(var_ref); var_ref = NULL;
        free(var_alt); var_alt = NULL;
    }
    return altseq;
}

static inline int find_variant(const Vector *a, const Variant *v) {
    int i = 0;
    int j = a->size - 1;
    int n = (i + j) / 2;
    while (i <= j) {
        Variant *curr = (Variant *)a->data[n];
        if (strcmp(v->chr, curr->chr) == 0 && strcmp(v->ref, curr->ref) == 0 && strcmp(v->alt, curr->alt) == 0 && v->pos == curr->pos) return n;
        if (v->pos > curr->pos) i = n + 1;
        else j = n - 1;
        n = (i + j) / 2;
    }
    return -1;
}

static inline void print_variant(char **output, const Vector *var_vector, size_t i, int read_count, int has_alt_count, double total, double has_alt, double not_alt) {
    Variant **var_set = (Variant **)var_vector->data;
    size_t nvariants = var_vector->size;

    size_t n;
    char *token;
    double prob = (has_alt - total) * M_1_LN10;
    double odds = (has_alt - not_alt) * M_1_LN10;

    n = snprintf(NULL, 0, "%s\t%d\t%s\t%s\t%d\t%d\t%e\t%f\t", var_set[i]->chr, var_set[i]->pos, var_set[i]->ref, var_set[i]->alt, read_count, has_alt_count, prob, odds) + 1;
    token = malloc(n * sizeof *token);
    snprintf(token, n, "%s\t%d\t%s\t%s\t%d\t%d\t%e\t%f\t", var_set[i]->chr, var_set[i]->pos, var_set[i]->ref, var_set[i]->alt, read_count, has_alt_count, prob, odds);
    str_resize(output, strlen(*output) + n);
    strcat(*output, token);
    free(token); token = NULL;

    str_resize(output, strlen(*output) + 2);
    strcat(*output, "[");
    if (nvariants > 1) {
        for (i = 0; i < nvariants; ++i) {
            n = snprintf(NULL, 0, "%d,%s,%s;", var_set[i]->pos, var_set[i]->ref, var_set[i]->alt) + 1;
            token = malloc(n * sizeof *token);
            snprintf(token, n, "%d,%s,%s;", var_set[i]->pos, var_set[i]->ref, var_set[i]->alt);
            str_resize(output, strlen(*output) + n);
            strcat(*output, token);
            free(token); token = NULL;
        }
    }
    str_resize(output, strlen(*output) + 3);
    strcat(*output, "]\n");
}

static char *evaluate_variants(const Vector *var_vector, const char *bam_file, const char *fa_file) {
    size_t i, n, seti, readi;

    Variant **var_set = (Variant **)var_vector->data;
    size_t nvariants = var_vector->size;

    /* Reads in variant region coordinates (vcf is 1-index while htslib is 0-index) */
    n = snprintf(NULL, 0, "%s:%d-%d", var_set[0]->chr, var_set[0]->pos - 1, var_set[nvariants - 1]->pos - 1) + 1;
    char *region = malloc(n * sizeof *region);
    snprintf(region, n, "%s:%d-%d", var_set[0]->chr, var_set[0]->pos - 1, var_set[nvariants - 1]->pos - 1);

    Vector *read_vector = fetch_reads(bam_file, region);
    free(region); region = NULL;
    if (read_vector->size == 0) {
        free(read_vector); read_vector = NULL;
        return NULL;
    }
    Read **read_set = (Read **)read_vector->data;
    size_t nreads = read_vector->size;

    /* Variant combinations as a tab delimited string, encoding array indices ('\t'+1)-indexed, then parsed into a an array of Vectors */
    int ncombos = 0;
    char *combo = powerset(nvariants, &ncombos);

    Vector **var_combo = malloc(ncombos * sizeof (Vector *)); 
    int n1;
    seti = 0;
    char *s1, *s2, token[strlen(combo)];
    for (s1 = combo; sscanf(s1, "%[^\t]%n", token, &n1) == 1 && seti < ncombos; s1 += n1 + 1) {
        var_combo[seti] = vector_create(nvariants, VARIANT_T);
        for (s2 = token; *s2 != '\0'; ++s2)  vector_add(var_combo[seti], var_set[*s2 - '\t' - 1]);
        ++seti;
        if (*(s1 + n1) != '\t') break;
    }
    free(combo); combo = NULL;
    //for (seti = 0; seti < ncombos; ++seti) { printf("%d\t", seti); for (i = 0; i < var_combo[seti]->size; ++i) { Variant *curr = (Variant *)var_combo[seti]->data[i]; printf("%s,%d,%s,%s\t", curr->chr, curr->pos, curr->ref, curr->alt); } printf("\n"); }

    /* Reference sequence */
    Fasta *f = fetch_refseq(var_set[0]->chr, fa_file);
    char *refseq = f->seq;
    int refseq_length = f->seq_length;

    /* Prior probabilities based on variant combinations */
    double alt_prior, het_prior;
    if (nvariants == 1 || mvh) {
        alt_prior = log(0.5 * (1 - hetbias));
        het_prior = log(0.5 * hetbias);
    }
    else {
        alt_prior = log(0.5 * (1 - hetbias) / ncombos);
        het_prior = log(0.5 * hetbias / ncombos);
    }

    double ref = 0;
    double alt[ncombos], het[ncombos];
    double pout[nreads], prgu[nreads];
    int ref_count[ncombos], alt_count[ncombos];
    for (seti = 0; seti < ncombos; ++seti) {
        alt[seti] = 0;
        het[seti] = 0;
        ref_count[seti] = 0;
        alt_count[seti] = 0;

        /* Alternative sequence */
        int altseq_length = 0;
        char *altseq = construct_altseq(refseq, refseq_length, var_combo[seti], &altseq_length);

        /* Aligned reads */
        for (readi = 0; readi < nreads; ++readi) {
            int is_unmap = 0;
            int is_reverse = 0;
            int is_secondary = 0;
            int n;
            char *s, token[64];
            for (s = read_set[readi]->flag; sscanf(s, "%63[^,]%n", token, &n) == 1; s += n + 1) {
                if (strcmp("UNMAP", token) == 0) is_unmap = 1;
                else if (strcmp("REVERSE", token) == 0) is_reverse = 1;
                else if (strcmp("SECONDARY", token) == 0 || strcmp("SUPPLEMENTARY", token) == 0) is_secondary = 1;
                if (*(s + n) != ',') break;
            }
            if (is_unmap) continue;
            if (pao && is_secondary) continue;

            /* Read probability matrix */
            double is_match[read_set[readi]->length], no_match[read_set[readi]->length];
            for (i = 0; i < read_set[readi]->length; ++i) {
                if (read_set[readi]->qual[i] == 0) read_set[readi]->qual[i] = -0.01;
                double a = read_set[readi]->qual[i] * M_1_LOG10E; //convert to ln
                is_match[i] = log(1 - exp(a)); // log(1-err)
                no_match[i] = a - LG3; // log(err/3)
            }
            double readprobmatrix[read_set[readi]->length * 5];
            set_prob_matrix(readprobmatrix, read_set[readi]->qseq, read_set[readi]->length, is_match, no_match);

            double elsewhere = 0;
            if (seti == 0) { // reference genome probability and "elsewhere" probability only needs to be calculated once
                /* 
                Probability of read is from an outside paralogous "elsewhere". Approximate the probability distribution by accounting for the bulk with:
                   a) perfect match = prod[ (1-e) ]
                   b) hamming distance 1 = prod[ (1-e) ] * sum[ (e/3) / (1-e) ]
                Also account for if reads have different lengths (hard clipped), where longer reads should have a relatively lower probability:
                   c) lengthfactor = alpha ^ (read length - expected read length)
                P(elsewhere) = (perfect + hamming) / lengthfactor 
                */
                double delta[read_set[readi]->length];
                double a = sum(is_match, read_set[readi]->length);
                for (i = 0; i < read_set[readi]->length; ++i) delta[i] = no_match[i] - is_match[i];
                elsewhere = log_add_exp(a, a + log_sum_exp(delta, read_set[readi]->length)) - (LGALPHA * (read_set[readi]->length - read_set[readi]->inferred_length));
                pout[readi] = elsewhere;
                /* Calculate the probability given reference genome */
                prgu[readi] = calc_prob_distrib(readprobmatrix, read_set[readi]->length, refseq, refseq_length, read_set[readi]->pos);
            }
            /* Calculate the probability given alternative genome */
            double prgv = calc_prob_distrib(readprobmatrix, read_set[readi]->length, altseq, altseq_length, read_set[readi]->pos);
            if (debug) {
                fprintf(stderr, "%d:\t%f\t%f\t%f\t", (int)seti, prgu[readi], prgv, pout[readi]);
                fprintf(stderr, "%s\t%s\t%d\t%d\t%d\t", read_set[readi]->name, read_set[readi]->chr, read_set[readi]->pos, read_set[readi]->length, read_set[readi]->inferred_length);
                fprintf(stderr, "%s\t", read_set[readi]->qseq);
                for (i = 0; i < read_set[readi]->length; ++i) fprintf(stderr, "%.2f ", read_set[readi]->qual[i]);
                fprintf(stderr, "\t");
                if (read_set[readi]->flag != NULL) fprintf(stderr, "%s\t", read_set[readi]->flag);
                if (read_set[readi]->multimap != NULL) fprintf(stderr, "%s\t", read_set[readi]->multimap);
                for (i = 0; i < read_set[readi]->n_cigar; ++i) fprintf(stderr, "%d%c ", read_set[readi]->cigar_oplen[i], read_set[readi]->cigar_opchr[i]);
                fprintf(stderr, "\n");
            }

            /* Multi-map alignments from XA tags: Zchr8,+42860367,97M3S,3;chr9,-44165038,100M,4; */
            if (!pao && read_set[readi]->multimap != NULL) {
                int xa_pos, n;
                char xa_chr[64];
                for (s = read_set[readi]->multimap + 1; sscanf(s, "%63[^,],%d,%*[^;]%n", xa_chr, &xa_pos, &n) == 2; s += n + 1) {
                    if (*(s + n) != ';') break;

                    Fasta *f = fetch_refseq(xa_chr, fa_file);
                    char *xa_refseq = f->seq;
                    int xa_refseq_length = f->seq_length;

                    double *p_readprobmatrix = readprobmatrix;
                    double newreadprobmatrix[read_set[readi]->length * 5];
                    if ((xa_pos < 0 && !is_reverse) || (xa_pos > 0 && is_reverse)) { // opposite of primary alignment strand
                        char *rev_qseq = reverse_compl(read_set[readi]->qseq, read_set[readi]->length);
                        double *rev_is_match = reverse(is_match, read_set[readi]->length);
                        double *rev_no_match = reverse(no_match, read_set[readi]->length);
                        set_prob_matrix(newreadprobmatrix, rev_qseq, read_set[readi]->length, rev_is_match, rev_no_match);
                        p_readprobmatrix = newreadprobmatrix;
                        free(rev_qseq); rev_qseq = NULL;
                        free(rev_is_match); rev_is_match = NULL;
                        free(rev_no_match); rev_no_match = NULL;
                    }

                    xa_pos = abs(xa_pos) - 1;
                    double readprobability = calc_prob_distrib(p_readprobmatrix, read_set[readi]->length, xa_refseq, xa_refseq_length, xa_pos);
                    if (seti == 0) {
                        pout[readi] = log_add_exp(pout[readi], elsewhere); // the more multi-mapped, the more likely it is the read is from elsewhere (paralogous), hence it scales (multiplied) with the number of multi-mapped locations
                        prgu[readi] = log_add_exp(prgu[readi], readprobability);
                    }
                    if (debug) fprintf(stderr, "%f\t", readprobability);
                    if (strcmp(xa_chr, read_set[readi]->chr) == 0) { // secondary alignments are in same chromosome as primary, check if it is near the variant position, otherwise is the same as probability given reference
                        if (abs(xa_pos - ((Variant *)var_combo[seti]->data[0])->pos) < 50) readprobability = calc_prob_distrib(p_readprobmatrix, read_set[readi]->length, altseq, altseq_length, xa_pos);
                    }
                    prgv = log_add_exp(prgv, readprobability);
                    if (debug) fprintf(stderr, "%f\t%f\t%f\n", readprobability, prgu[readi], prgv);
                }
            }

            /* Mixture model: probability that the read is from elsewhere, outside paralogous source */
            if (seti == 0) prgu[readi] = log_add_exp(LGOMEGA - LG1_OMEGA + pout[readi], prgu[readi]);
            prgv = log_add_exp(LGOMEGA - LG1_OMEGA + pout[readi], prgv);

            /* Mixture model: heterozygosity or heterogeneity as explicit allele frequency mu such that P(r|GuGv) = (mu)(P(r|Gv)) + (1-mu)(P(r|Gu)) */
            double phet   = log_add_exp(LG50 + prgv, LG50 + prgu[readi]);
            double phet10 = log_add_exp(LG10 + prgv, LG90 + prgu[readi]);
            double phet90 = log_add_exp(LG90 + prgv, LG10 + prgu[readi]);
            if (phet10 > phet) phet = phet10;
            if (phet90 > phet) phet = phet90;

            /* Read count incremented only when the difference in probability is not ambiguous, > ~log(2) difference */
            if (prgv > prgu[readi] && prgv - prgu[readi] > 0.69) alt_count[seti] += 1;
            else if (prgu[readi] > prgv && prgu[readi] - prgv > 0.69) ref_count[seti] += 1;

            /* Priors */
            if (seti == 0) ref += prgu[readi] + REFPRIOR;
            alt[seti] += prgv + alt_prior;
            het[seti] += phet + het_prior;
            if (debug) {
                fprintf(stderr, "%d\t++\t%f\t%f\t%f\t%f\t%d\t%s\t", (int)seti, prgu[readi], phet, prgv, pout[readi], alt_count[seti], read_set[readi]->name);
                for (i = 0; i < var_combo[seti]->size; ++i) { Variant *curr = (Variant *)var_combo[seti]->data[i]; fprintf(stderr, "%s,%d,%s,%s;", curr->chr, curr->pos, curr->ref, curr->alt); } fprintf(stderr, "\n");
            }
        }
        free(altseq); altseq = NULL;
        if (debug) {
            fprintf(stderr, "%d\t==\t%f\t%f\t%f\t%d\t", (int)seti, ref, het[seti], alt[seti], alt_count[seti]);
            for (i = 0; i < var_combo[seti]->size; ++i) { Variant *curr = (Variant *)var_combo[seti]->data[i]; fprintf(stderr, "%s,%d,%s,%s;", curr->chr, curr->pos, curr->ref, curr->alt); } fprintf(stderr, "\n");
        }
    }

    /* Marginal probabilities */
    double total = ref;
    int max_ref_count = 0;
    int max_alt_count = 0;
    for (seti = 0; seti < ncombos; ++seti) {
        total = log_add_exp(ref, log_add_exp(alt[seti], het[seti]));
        if (ref_count[seti] > max_ref_count) max_ref_count = ref_count[seti];
        if (alt_count[seti] > max_alt_count) max_alt_count = alt_count[seti];
    }
    int read_count = max_ref_count + max_alt_count;

    char *output = malloc(sizeof *output);
    output[0] = '\0';
    for (i = 0; i < nvariants; ++i) {
        double has_alt = 0;
        double not_alt = ref;
        int has_alt_count = 0;
        for (seti = 0; seti < ncombos; ++seti) {
            if (find_variant(var_combo[seti], var_set[i]) != -1) { // if variant is in this combination
                has_alt = has_alt == 0 ? log_add_exp(alt[seti], het[seti]) : log_add_exp(has_alt, log_add_exp(alt[seti], het[seti]));
                if (alt_count[seti] > has_alt_count) has_alt_count = alt_count[seti];
            }
            else {
                not_alt = log_add_exp(not_alt, log_add_exp(alt[seti], het[seti]));
            }
        }
        print_variant(&output, var_vector, i, read_count, has_alt_count, total, has_alt, not_alt);
    }
    vector_destroy(read_vector); free(read_vector); read_vector = NULL;
    for (seti = 0; seti < ncombos; ++seti) { vector_free(var_combo[seti]); free(var_combo[seti]); var_combo[seti] = NULL; }
    free(var_combo); var_combo = NULL;
    return output;
}

typedef struct {
    Vector *var_set;
    char *bam_file, *fa_file;
} FuncArg;

typedef struct {
    Vector *queue, *results;
    pthread_mutex_t q_lock;
    pthread_mutex_t r_lock;
} Work;

static void *pool(void *work) {
    Work *w = (Work *)work;
    Vector *queue = (Vector *)w->queue;
    Vector *results = (Vector *)w->results;

    while (1) { //pthread_t ptid = pthread_self(); uint64_t threadid = 0; memcpy(&threadid, &ptid, min(sizeof(threadid), sizeof(ptid)));
        pthread_mutex_lock(&w->q_lock);
        FuncArg *arg = (FuncArg *)vector_pop(queue);
        pthread_mutex_unlock(&w->q_lock);
        if (arg == NULL) break;
        
        char *outstr = evaluate_variants(arg->var_set, arg->bam_file, arg->fa_file);
        if (outstr == NULL) continue;

        pthread_mutex_lock(&w->r_lock);
        vector_add(results, outstr);
        vector_free(arg->var_set); free(arg->var_set); arg->var_set = NULL;
        free(arg); arg = NULL;
        pthread_mutex_unlock(&w->r_lock);
    }
    return NULL;
}

void process_variants(const Vector *var_vector, char *bam_file, char *fa_file, FILE *out_fh) {
    size_t i, j;

    Variant **varlist = (Variant **)var_vector->data;
    size_t nvariants = var_vector->size;

    /* Variants that are close together as sets */
    i = 0;
    size_t nsets = 0;
    Vector **var_set = malloc((nvariants * 2)  * sizeof (Vector *));
    while (i < nvariants) {
        Vector *curr = vector_create(8, VARIANT_T);
        vector_add(curr, varlist[i]);
        size_t j = i + 1;
        while (distlim > 0 && j < nvariants && strcmp(varlist[j]->chr, varlist[j - 1]->chr) == 0 && abs(varlist[j]->pos - varlist[j - 1]->pos) <= distlim) vector_add(curr, varlist[j++]);
        i = j;
        var_set[nsets++] = curr;
    }
    /* Heterozygous non-reference variants as separate entries */
    int flag = 1;
    while (flag) {
        flag = 0;
        size_t n = 0;
        for (i = 0; i < nsets; ++i) {
            if (var_set[i]->size == 1) continue;
            for (j = 0; j < var_set[i]->size - 1; ++j) {
                Variant *curr = (Variant *)var_set[i]->data[j];
                Variant *next = (Variant *)var_set[i]->data[j + 1];
                if (curr->pos == next->pos) {
                    flag = 1;
                    Vector *dup = vector_dup(var_set[i]);
                    vector_del(var_set[i], j);
                    vector_del(dup, j + 1);
                    var_set[nsets + n++] = dup;
                }
            }
        }
        nsets += n;
    } 
    print_status("Variants within %d bp:\t%i entries\t%s", distlim, (int)nsets, asctime(time_info));

    print_status("Start:\t%d procs \t%s\t%s", numproc, bam_file, asctime(time_info));
    Vector *queue = vector_create(nsets, VOID_T);
    Vector *results = vector_create(nsets, VOID_T);
    for (i = 0; i < nsets; ++i) {
        FuncArg *arg = malloc(sizeof (FuncArg));
        arg->var_set = var_set[i];
        arg->bam_file = bam_file;
        arg->fa_file = fa_file;
        vector_add(queue, arg);
    }
    Work *w = malloc(sizeof (Work));
    w->queue = queue;
    w->results = results;
    pthread_mutex_init(&w->q_lock, NULL);
    pthread_mutex_init(&w->r_lock, NULL);

    pthread_t tid[numproc];
    for (i = 0; i < numproc; ++i) pthread_create(&tid[i], NULL, pool, w);
    for (i = 0; i < numproc; ++i) pthread_join(tid[i], NULL);

    pthread_mutex_destroy(&w->q_lock);
    pthread_mutex_destroy(&w->r_lock);
    free(w); w = NULL;
    free(var_set); var_set = NULL;

    qsort(results->data, results->size, sizeof (void *), nat_sort_str);
    fprintf(out_fh, "#SEQ\tPOS\tREF\tALT\tReads\tAltReads\tProb\tOdds\tSet\n");
    for (i = 0; i < results->size; ++i) fprintf(out_fh, "%s", (char *)results->data[i]);
    vector_destroy(queue); free(queue); queue = NULL;
    vector_destroy(results); free(results); results = NULL;
    print_status("Done:\t%s\t%s", bam_file, asctime(time_info));
}

static void print_usage() {
    printf("\n");
    printf("Usage: eagle [options] -v variants.vcf -a alignment.bam -r reference.fasta\n");
    printf("\n");
    printf("Required:\n");
    printf("  -v --vcf     FILE   variants VCF file\n");
    printf("  -a --bam     FILE   alignment data bam files (ref coord sorted and indexed)\n");
    printf("  -r --ref     FILE   reference sequence fasta file\n");
    printf("Options:\n");
    printf("  -o --out     FILE   output file (default: stdout)\n");
    printf("  -t --numproc INT    number of processes to use (default: 1)\n");
    printf("  -n --distlim INT    consider nearby variants within n bases as a set of hypotheses (off: 0, default: 10)\n");
    printf("  -m --maxh    INT    the maximum number of combinations in the set of hypotheses, instead of all 2^n (default: 2^10 = 1024)\n");
    printf("     --mvh            consider nearby variants as *one* multi-variant hypothesis\n");
    printf("  -b --hetbias FLOAT  prior probability bias towards non-homozygous mutations (value between [0,1], default: 0.5 unbiased)\n");
    printf("     --pao            consider primary alignments only\n");
}

// gcc -Ihtslib -Lhtslib eagle.c htslib/libhts.a -lm -lz -lpthread -o eagle -g -O0
// valgrind --leak-check=yes --track-origins=yes
// valgrind --tool=drd --check-stack-var=yes
int main(int argc, char *argv[]) {
    /* Command line parameters defaults */
    char *vcf_file = NULL;
    char *bam_file = NULL;
    char *fa_file = NULL;
    char *out_file = NULL;
    numproc = 1;
    distlim = 10;
    maxh = 1024;
    mvh = 0;
    hetbias = 0.5;
    pao = 0;
    debug = 0;

    static struct option long_options[] = {
        {"vcf", required_argument, 0, 'v'},
        {"bam", required_argument, 0, 'a'},
        {"ref", required_argument, 0, 'r'},
        {"out", optional_argument, 0, 'o'},
        {"numproc", optional_argument, 0, 't'},
        {"distlim", optional_argument, 0, 'n'},
        {"hetbias", optional_argument, 0, 'b'},
        {"maxh", optional_argument, 0, 'm'},
        {"mvh", no_argument, &mvh, 1},
        {"pao", no_argument, &pao, 1},
        {"debug", no_argument, &debug, 1},
        {0, 0, 0, 0}
    };

    int opt = 0;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv,"v:a:r:o:t:n:b:m:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 0: 
                //if (long_options[option_index].flag != 0) break;
                break;
            case 'v': vcf_file = optarg; break;
            case 'a': bam_file = optarg; break;
            case 'r': fa_file= optarg; break;
            case 'o': out_file = optarg; break;
            case 't': numproc = parse_num(optarg); break;
            case 'n': distlim = parse_num(optarg); break;
            case 'b': hetbias = parse_num(optarg); break;
            case 'm': maxh = parse_num(optarg); break;
            default: exit_usage("Bad program call");
        }
    }
    if (vcf_file == NULL) { exit_usage("Missing variants given as VCF file!"); }
    if (bam_file == NULL) { exit_usage("Missing alignments given as BAM file!"); }
    if (fa_file == NULL) { exit_usage("Missing reference genome given as Fasta file!"); }
    if (numproc < 1) numproc = 1;
    if (distlim < 0) distlim = 0;
    if (hetbias < 0 || hetbias > 1) hetbias = 0.5;
    if (maxh < 0) maxh = 1024;

    FILE *out_fh;
    if (out_file == NULL) out_fh = stdout;
    else out_fh = fopen(out_file, "w");

    fprintf(stderr, "VCF: %s\nBAM: %s\nREF: %s\n", vcf_file, bam_file, fa_file); fprintf(stderr, "numproc: %d, distlim: %d, hetbias: %.2f, maxh: %d\n", numproc, distlim, hetbias, maxh); fprintf(stderr, "mvh: %d, pao: %d, debug: %d\n\n", mvh, pao, debug);

    /* Mapping table */
    memset(seqnt_map, 4, sizeof seqnt_map);
    seqnt_map['A'-'A'] = 0;
    seqnt_map['T'-'A'] = 1;
    seqnt_map['G'-'A'] = 2;
    seqnt_map['C'-'A'] = 3;
    seqnt_map['N'-'A'] = 4;

    memset(compl_map, 0, sizeof compl_map);
    compl_map['A'-'A'] = 'T';
    compl_map['T'-'A'] = 'A';
    compl_map['C'-'A'] = 'G';
    compl_map['G'-'A'] = 'C';
    compl_map['N'-'A'] = 'N';

    /* Start processing data */
    Vector *var_vector = read_vcf(vcf_file);

    refseq_hash = kh_init(rsh);
    //read_fasta(fa_file);

    pthread_mutex_init(&refseq_lock, NULL);
    process_variants(var_vector, bam_file, fa_file, out_fh);
    pthread_mutex_destroy(&refseq_lock);

	khiter_t k;
    for (k = kh_begin(refseq_hash); k != kh_end(refseq_hash); ++k) {
		if (kh_exist(refseq_hash, k)) vector_destroy(&kh_val(refseq_hash, k));
    }
	kh_destroy(rsh, refseq_hash);
    vector_destroy(var_vector); free(var_vector); var_vector = NULL;
    return 0;
}