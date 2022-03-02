import pynic


@pynic.apply
def bar(x, y):
    return x * x + y * y


print(bar(3, 4))


@pynic.apply
def foo(x, y):
    return x * x + y * y


print(foo(3, 4))
