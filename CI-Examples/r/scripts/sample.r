# Draws random samples with sample() and checks their shape; prints "success" if all checks pass.

set.seed(1)

a <- sample(1:3)
stopifnot(length(a) == 3, setequal(a, 1:3))

b <- sample(c(2, 5, 3), size = 4, replace = TRUE)
stopifnot(length(b) == 4, all(b %in% c(2, 5, 3)))

d <- sample(1:2, size = 10, prob = c(1, 3), replace = TRUE)
stopifnot(length(d) == 10, all(d %in% 1:2))

print("success")
