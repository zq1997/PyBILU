from opcode import *

import pynic

import time


def stat(name, func, repeat, get_args):
    t0 = time.time()
    for i in range(repeat):
        func(*get_args())
    t1 = time.time()
    print('%s: %.3fs' % (name, t1 - t0), flush=True)


def foo(L):
    s = 0
    for i in L:
        s = s + i
    return s


# stat('无优化', foo, 1, lambda: [range(5000000)])
# stat('有优化', pynic.apply(foo), 1, lambda: [range(5000000)])

def bar(n):
    if n < 2:
        return n
    x = 0
    y = 1
    i = 1
    while i < n:
        f = x + y
        x = y
        y = f
        i = i + 1
    return y


# stat('无优化', bar, 111, lambda: [12345])
# stat('有优化', pynic.apply(bar), 111, lambda: [12345])

class A:
    def func(self, a, b):
        print('调用方法', a, b)


@pynic.apply
def test(d):
    def f(d):
        print('ok', d)
    f(d)
    # return d.func(1, 2)


print(test(A()))
