import pybilu

@pybilu.apply
def add(x, y):
    return x + y


print(add(3, 4))
