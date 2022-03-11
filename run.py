import pynic


# @pynic.apply
# def foo(x, y):
#     return x * x + y * y


@pynic.apply
def foo(x, y):
    xx = x * x
    yy = y * y
    return xx + yy


print(foo(3, 4))
