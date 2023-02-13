# Minimac4

Minimac4 is a lower memory and more computationally efficient
implementation of the genotype imputation algorithms in 
minimac/mininac2/minimac3.

<<< SEE http://genome.sph.umich.edu/wiki/Minimac4 FOR DOCUMENTATION >>>

## Prerequisites
Automatic installation of Minimac4 requires [cget](http://cget.readthedocs.io/en/latest/src/intro.html#installing-cget) and cmake >= v3.2.

## Installation
The easiest way to install Minimac4 and its dependencies is to use cget:
```bash
cget install --prefix <install_prefix> statgen/Minimac4
```

Alternatively, you can install manually:
```bash
cd Minimac4
cget install -f ./requirements.txt                      # Install dependencies locally.
mkdir build && cd build                                 # Create out of source build directory.
cmake -DCMAKE_TOOLCHAIN_FILE=../cget/cget/cget.cmake .. # Configure project with dependency paths.
make                                                    # Build.
make install                                            # Install
```

To build and run tests from build directory:
```bash
# bcftools is required to run tests
cmake -DCMAKE_TOOLCHAIN_FILE=../cget/cget/cget.cmake -DBUILD_TESTS=ON ..
make
make CTEST_OUTPUT_ON_FAILURE=1 test
```

Since some users have reported issues with installing cget with pip, a cmake-only alternative is available:
```shell
cmake -P dependencies.cmake deps/
mkdir build; cd build
cmake -DCMAKE_PREFIX_PATH=$(pwd)/../deps/ -DCMAKE_CXX_FLAGS="-I$(pwd)/../deps/include" ..
make
make install
```


## Usage
See `minimac4 --help` for detailed usage.

A typical Minimac4 command line for imputation is as follows
```bash
minimac4 reference.msav target.vcf.gz > imputed.sav
```

Here reference.msav is a reference panel (e.g. 1000 Genomes) compressed with MVCF encoding, 
target.vcf.gz is an indexed VCF containing phased genotype array data, 
and imputed.sav is the imputed output.

The file formats VCF, [BCF](https://github.com/samtools/bcftools), and [SAV](https://github.com/statgen/savvy) are supported for both input and output:
```bash
minimac4 reference.msav target.bcf -o imputed.bcf
minimac4 reference.msav target.vcf.gz -o imputed.vcf.gz
```

A sites-only file can be generated with:
```bash
minimac4 reference.msav target.bcf -o imputed.sav -s imputed.sites.vcf.gz
```

Meta-imputation with MetaMinimac2 requires `--empirical-output` (or `-e`) to be specified:
```bash
minimac4 reference.msav target.bcf -o imputed.dose.sav -e imputed.empirical_dose.sav
```
