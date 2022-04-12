from opcode import *

import pynic
# import time
#
#
# def stat(name, func, repeat, get_args):
#     t0 = time.time()
#     for i in range(repeat):
#         func(*get_args())
#     t1 = time.time()
#     print('%s: %.3fs' % (name, t1 - t0))


# def foo(L):
#     s = 0
#     for i in L:
#         s = s + i
#     return s
#
#
# @pynic.apply
# def bar(L):
#     s = 0
#     for i in L:
#         s = s + i
#     return s
#
#
# stat('无优化', foo, 1, lambda : [range(5000000)])
# stat('有优化', bar, 1, lambda : [range(5000000)])

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
#
#
# @pynic.apply
# def bar(n):
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
#
#
# stat('无优化', foo)
# stat('有优化', bar)

class I(int):
    def __del__(self):
        print("死了:", int(self))

@pynic.apply
def foo(a, b):
    return a + (a := b)
    # return x * x + x * x


print(foo(I(1997 * 1000000), I(623 * 1000)))
