import pynic


@pynic.apply
def foo(x, y):
    return x * x + y * y


@pynic.apply
def foo(x, y):
    return x * x + y * y

print(foo(3, 4))
