context("text_locate")


test_that("'text_count' can count instances", {
    text <- c("Rose is a rose is a rose is a rose.",
              "A rose by any other name would smell as sweet.",
              "Snow White and Rose Red")

    expect_equal(text_count(text, "is"), c(3, 0, 0))
    expect_equal(text_count(text, "a"), c(3, 1, 0))
    expect_equal(text_count(text, "rose"), c(4, 1, 1))
    expect_equal(text_count(text, c("is", "a", "rose")), c(10, 2, 1))
})


test_that("'text_count' can use a custom filter", {
    text <- c("Rose is a rose is a rose is a rose.",
              "A rose by any other name would smell as sweet.",
              "Snow White and Rose Red")
    f <- token_filter(map_case = FALSE)

    expect_equal(text_count(text, "rose", f), c(3, 1, 0))
})


test_that("'text_detect' can find instances", {
    text <- c("Rose is a rose is a rose is a rose.",
              "A rose by any other name would smell as sweet.",
              "Snow White and Rose Red")

    expect_equal(text_detect(text, "is"), c(TRUE, FALSE, FALSE))
    expect_equal(text_detect(text, "rose"), c(TRUE, TRUE, TRUE))
    expect_equal(text_detect(text, "a"), c(TRUE, TRUE, FALSE))
    expect_equal(text_detect(text, c("is", "a", "rose")), c(TRUE, TRUE, TRUE))
})


test_that("'text_detect' can use a custom filter", {
    text <- c("Rose is a rose is a rose is a rose.",
              "A rose by any other name would smell as sweet.",
              "Snow White and Rose Red")
    f <- token_filter(map_case = FALSE)

    expect_equal(text_detect(text, "rose", f), c(TRUE, TRUE, FALSE))
})


test_that("'text_locate' can give instance contexts", {
    text <- c("Rose is a rose is a rose is a rose.",
              "A rose by any other name would smell as sweet.",
              "Snow White and Rose Red")

    actual <- text_locate(text, "rose")

    expected <- data.frame(
        text = c(1, 1, 1, 1, 2, 3),
        term = rep("rose", 6),
        before = as_text(c("", "Rose is a ", "Rose is a rose is a ",
                   "Rose is a rose is a rose is a ", "A ",
                   "Snow White and ")),
        instance = c("Rose", "rose", "rose", "rose", "rose", "Rose"),
        after = as_text(c(" is a rose is a rose is a rose.",
                          " is a rose is a rose.", " is a rose.", ".",
                          " by any other name would smell as sweet.",
                          " Red")),
        stringsAsFactors = FALSE)
    class(expected) <- c("corpus_text_locate", "data.frame")

    expect_equal(actual, expected)
})


test_that("'text_locate' can use a custom filter", {
    text <- c("Rose is a rose is a rose is a rose.",
              "A rose by any other name would smell as sweet.",
              "Snow White and Rose Red")
    f <- token_filter(map_case = FALSE)
    actual <- text_locate(text, "Rose", f)

    expected <- data.frame(
        text = c(1, 3),
        term = rep("Rose", 2),
        before = as_text(c("", "Snow White and ")),
        instance = c("Rose", "Rose"),
        after = as_text(c(" is a rose is a rose is a rose.", " Red")),
        stringsAsFactors = FALSE)
    class(expected) <- c("corpus_text_locate", "data.frame")

    expect_equal(actual, expected)
})