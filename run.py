import pynic
import time


def foo(L):
    s = 0
    for i in L:
        s = s + i
    return s


@pynic.apply
def bar(L):
    s = 0
    for i in L:
        s = s + i
    return s


def stat(name, func):
    t0 = time.time()
    func(range(500000))
    t1 = time.time()
    print('%s: %.3fs' % (name, t1 - t0))


stat('无优化', foo)
stat('有优化', bar)

# 有bug
# @pynic.apply
# def foo(n):
#     if n < 2:
#         return n
#     x = 0
#     y = 1
#     i = 1
#     while i < n:
#         f = x + y
#         x = y
#         y = f
#         i = i + 1
#     return y
