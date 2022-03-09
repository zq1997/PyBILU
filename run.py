import pynic


# @pynic.apply
# def foo(x, y):
#     return x * x + y * y


@pynic.apply
def foo(x, y):
    return x + y


print(foo(int('114510'), 4))
