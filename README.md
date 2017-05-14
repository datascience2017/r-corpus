Corpus (R Package)
==================

Text corpus analysis in R. Heavy lifting is done by the
[Corpus C library][corpus].


Overview
--------

This is an R text processing package that currently does very little. It
exports three main functions:

 + `read_ndjson()` for reading in data in newline-delimited JSON format;

 + `sentences()` for segmenting text into sentences;

 + `tokens()` for segmenting text into tokens, each of which is an instance
    of a particular word type (normalized token).

The package also provides two new data types:

 + `jsondata` for storing JSON-encoded data;

 + `text` for storing text.

That's it. There are no n-grams, no term-by-document matrix, no part-of-speech
tagging, no topic models, and no word vectors.  Some of these features are
planned for future releases.


Installing
----------

Corpus is [available on CRAN][cran]. To install the latest released
version, run the following command in R:

    install.packages("corpus")

Alternatively, use any other other installation method supported by your
R development environment.


Demonstration
-------------

Here are some performance comparisons for some basic operations. We
compare against *jsonlite* version 1.3 and *stringi* version 1.1.3.


### Extracting text from a newline-delimited JSON file

The following benchmark reads in a 286 MB data file, `yelp-review.json`,
and extracts the text field from each row.  The raw data comes from the
first round of the [Yelp Dataset Challence][yelp]; it is stored in
[newline-delimited JSON format][ndjson].

    # Using the corpus library:

    system.time({
        data <- read_ndjson("yelp-review.json", mmap=TRUE)
    })

    ##   user  system elapsed
    ##  2.146   0.108   2.257

    pryr::mem_used()

    ## 63.2 MB


    # Using the jsonlite library:

    system.time({
        data <- jsonlite::stream_in(file("yelp-review.json"), verbose=FALSE)
    })

    ##   user  system elapsed
    ## 36.788   0.588  37.522

    pryr::mem_used()

    ## 335 MB

We are about 16 times faster than *jsonlite*, and we use less than 20%
of the RAM.  How are we reading a 286 MB file in only 63.2 MB?  We memory-map
the file, letting the operating system move data from the file to RAM
whenever necessary. We store the addresses of the text strings, but we
do not load values into RAM until they are needed.

(If we specify `mmap=FALSE`, the default, then we read the entire file
into memory; in this case, `read_ndjson` is about 7 times faster than
*jsonlite* and uses about 10% more memory.)


### Segmenting text into sentences

The next benchmark segments each text into sentences, using the boundaries
defined by [Unicode Standard Annex #29, Section 5][sentbreak].

    # Using the corpus library:

    system.time({
        sents <- sentences(data$text)
    })

    ##   user  system elapsed
    ##  2.115   0.082   2.200

    rm("data")
    pryr::mem_used()
    ## 135 MB


    # Using the stringi library:
    system.time({
        sents <- stringi::stri_split_boundaries(data$text, type="sentence")
    })

    ##   user  system elapsed
    ##  8.010   0.147   8.191

    rm("data")
    pryr::mem_used()

    ## 536 MB

This is not an apples-to-apples comparison, because *corpus* returns a
single data frame with all of the sentences while *stringi* returns a list of
character vectors. This difference in output formats gives *corpus* a
speed advantage. A second difference is that in the *corpus* benchmark,
the input text object is an array of memory-mapped values returned by the call
to `read_ndjson`; in the *stringi* benchmark, the input text object is an
in-memory array returned by *jsonlite*. The *stringi* package
can read the values directly from RAM instead of from the hard drive.
Overall, *corpus* is about 3.7 times faster than *stringi*.


### Tokenizing and normalizing text

The next benchmark performs a series of operations to transform each text into
a sequence of normalized tokens, called types. First, we segment each text
into word tokens using the boundaries defined by
[Unicode Standard Annex #29, Section 4][wordbreak]. Next, we normalize the
text into [Unicode NFKC normal form][nfkc], and we [case fold][casefold]
the text (for most languages, replacing uppercase with lowercase). Then,
we remove non-white-space [control characters][cc], default ignorable
characters like zero-width spaces, and we remove white space. We also
"dash fold," replacing Unicode dashes with an ASCII dash (-), and we
"quote fold," replacing Unicode quotes (single, double, apostrophe) with
ASCII single quote ('). If, after normalization, the token is empty (for
example if it started out as white space), then we discard it.

    system.time({
        toks <- tokens(data$text)
    })

    ##   user  system elapsed
    ##  7.408   0.195   7.612

    rm("data")
    pryr::mem_used()

    ## 451 MB


With *stringi*, it is more efficient to do the word segmentation after the
normalization. Further, the *stringi* package handles punctuation and
white space differently, so it gives slightly different results.

    system.time({
        ntext <- stringi::stri_trans_nfkc_casefold(data$text)
        toks <- stringi::stri_extract_all_words(ntext)
    })

    ##   user  system elapsed
    ## 13.565   0.272  13.864

    rm("data", "ntext")
    pryr::mem_used()

    ## 400 MB

Both *corpus* and *stringi* produce the same type of output: lists of
character arrays.  As in the previous benchmark, *corpus* takes as input a
memory-mapped text object, while *stringi* takes as input in in-memory
character vector. We are 1.8 times faster than `stringi`. We use slightly more
RAM, but only because we do not throw away punctuation-only tokens.


Building from source
--------------------

To install the latest development version of the package, run the following
sequence of commands in R:

    local({
        dir <- tempfile()
        cmd <- paste("git clone --recursive",
                     shQuote("https://github.com/patperry/r-corpus.git"),
                     shQuote(dir))
        system(cmd)
        devtools::install(dir)
        devtools::test(dir) # run the tests (optional)
        unlink(dir, recursive=TRUE)
    })

Note that the package uses a git submodule, so you cannot use
`devtools::install_github` to install it.


To obtain the source code, clone the repository and the submodules:

    git clone --recursive https://github.com/patperry/r-corpus.git

The `--recursive` flag is to make sure that the corpus library also gets
cloned. If you forget the `--recursive` flag, you can manually clone
the submodule with the following command:

    git submodule update --init

There are no other dependencies.


License
-------

Corpus is released under the [Apache Licence, Version 2.0][apache].


[apache]: https://www.apache.org/licenses/LICENSE-2.0.html
[casefold]: https://www.w3.org/International/wiki/Case_folding
[cc]: https://en.wikipedia.org/wiki/C0_and_C1_control_codes
[corpus]: https://github.com/patperry/corpus
[cran]: https://cran.r-project.org/package=corpus
[ndjson]: http://ndjson.org/
[nfkc]: http://unicode.org/reports/tr15/
[sentbreak]: http://unicode.org/reports/tr29/#Sentence_Boundaries
[windows]: https://github.com/patperry/corpus/blob/master/TODO.md
[wordbreak]: http://unicode.org/reports/tr29/#Word_Boundaries
[yelp]: https://www.yelp.com/dataset_challenge
