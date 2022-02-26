import pybilu


@pybilu.apply
def id1(x):
    return x


@pybilu.apply
def id2(x):
    return x


print(id2(sum([100, 114414])))
