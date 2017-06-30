# EAGLE: Explicit Alternative Genome Likelihood Evaluator

**Requires**: htslib (http://www.htslib.org/). Set HTSDIR in the make file to the htslib folder and make.  Note that we merely call make on htslib, as such, its dependencies and system requirements need to be fufilled.

Usage: 

`eagle -v variants.vcf -a alignment.bam -r reference.fasta > output.tab`

### For EAGLE read classification, see EAGLE-RC below

**Inputs**

1. Alignment data in coordinated sorted indexed BAM format (\*\.bam, \*\.bam.bai)
2. Reference genome in FASTA format
3. Variant candidates in VCF format

**Output**

A tab-delimited text file with one row per variant and columns representing:

1. chromosome / sequence id
2. coordinate position
3. reference sequence
4. alternative sequence
5. total number of reads seen
6. number of reads supporting reference sequence
7. number of reads supporting alternative sequence
8. log 10 probability
9. log 10 likelihood ratio (odds)
10. variants in the set of nearby variant if any, otherwise []

The read counts represent reads that are unambiguously for the reference or alternative sequence (2x the probability favoring one over the other), as opposed to aligned reads. Our model attempts to account for various uncertainties in the hypotheses.

**Input/Output Parameters**

-v  VCF file describing the variants, only the columns describing position and sequence are used [columns: 1,2,4,5].

-a  BAM alignment data file, reference coordinated sorted with index [*filename*.bam.bai].

-r  Reference genome [multi] fasta file.

-o  Output file name, defaults to *stdout* if not specified.

**Program Parameters**

-t INT  The number of processes to use.  Default is 1.

-s INT  Group/chain nearby variants based on shared reads.  Default is 0/off, which uses the distance based method.  Option 1 will group variants if any read that crosses the first variant also cross the variant under consideration.  Option 2 will group variants if any read that crosses any variant in the set also cross the variant under consideration.

-n INT  Group/chain nearby variants within *n* bp of each other to be considered in the set of hypotheses for marginal probability calculations.  Default is 10 bp (0 is off).

-w INT  Maximum number of bases between any two variants in the set of hypotheses. This sets a window size that allows for larger values of -n without chaining an excessive number of variants.  Default is 0 bp (off).

--maxh INT  The maximum number of hypotheses to be tested.  Instead of looking at all 2^n combinations for a set of variants, if after the current *k* for *n choose k* combinations is considered and the number of tested hypthotheses exceeds the maximum, then do not consider more combinations.  The solo variants and the "all variants" hypothesis are always tested first and do not count towards the maximum.  Default is 1024 (2^10).

--mvh  Instead of the marginal probabilities over the hypotheses set, output only the maximum variant hypothesis (highest probability of phased variants) in the set of hypotheses.  Note that *maxh* will limit the possible combinations tested.

--pao  Use primary alignments only, based on SAM flag.

--isc  Ignore soft-clipped bases in reads when calculating the probabilities, based on cigar string.

--nodup  Ignore marked duplicate reads, based on SAM flag.

--splice  Allow spliced reads, based on cigar string.

--dp  Instead of the short read model, which assumes no indel errors, use dynamic programming (short in long) to calculate the likelihood.  This allows handling of long reads which have higher rates of sequencing errors and indel errors.

--match INT  Matching score for use with *dp*.  Default is 2.

--mismatch INT  Mismatch penalty for use with *dp*.  Default is 5.

--gap\_op INT  Gap open penalty for use with *dp*.  Default is 2.

--gap\_ex INT  Gap extend penalty for use with *dp*.  Default is 1.

--verbose  Verbose mode.  Output the likelihoods for every read seen for every hypothesis to *stderr*.

--hetbias FLOAT  Prior probability bias towards heterozygous or homozygous mutations.  Value between [0,1] where 1 is towards heterozygosity.  Default is 0.5 (unbiased).

--omega FLOAT  Prior probability of originating from outside paralogous source.  Value between [0,1].  Default is 1e-5.

**Usage Notes**

*compare2TruthData.py* is a simple script to separate false positives and true positives based on truth data given as a VCF. 

*compileLikelihoods.py* post-processes the probabilities calculated by EAGLE and can be used to find somatic mutations given positive (i.e. tumor) and negative (i.e. normal) results on the same set of variants. Likelihood ratio and allele frequency thresholds are then used to filter mutations.

Heterozygous non-reference variants (VCF: comma separated multiple alternative sequences) are output as separate entries. Furthermore, if they are near other variants, it will separately consider each alternative sequence in their own sets so that phasing is not an issue. This may result in entries with the first 4 columns being identical. The user will need to examine the variant set of these cases to determine which it belongs to. The script *compileLikelihood.py* will naively retain the one with the maximum probability, for entries with identical coordinates.

## EAGLE-RC

EAGLE-RC is a method for classifying whether a read belongs to one genomic hypothesis or another, given a set of genomic differences between them.  This can be applicable for determining if reads originate from a specific allele or from a specific homeolog in allopolyploids.

First, use EAGLE to calculate likelihoods for each read and for each hypothesis (--verbose) as well as phased variants (--mvh) as output.  Then the program readclassify can take these two inputs to classify reads and optionally split reads into bam files for each class.  We also use a lower omega to be more tolerant to sequence differences outside the tested hypotheses.  Other options (such as --dp for long reads, --splice for RNA-seq, etc.) may or may not be applicable depending on the use case.

Usage: 

`eagle -t 2 -v variants.vcf -a alignment.bam -r reference.fasta --omega=1e-40 --mvh --verbose 1> output.tab 2>readinfo.txt`

`readclassify -a alignment.bam -o out_prefix output.tab readinfo.txt > classified_reads.list`

**Program Parameters**

-o  prefix for output BAM files.

--listonly  print classified read list only (stdout) without processing BAM files

--readlist  read from classified read list instead of EAGLE outputs, perhaps from a previous run or a merged file, and process BAM files.
