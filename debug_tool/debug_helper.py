import os
import struct
from opcode import EXTENDED_ARG, HAVE_ARGUMENT, opname, cmp_op, \
    haslocal, hasconst, hasname, hasfree, hasjabs, hasjrel, hascompare


def get_save_prefix(co):
    py_dir, py_file = os.path.split(os.path.abspath(co.co_filename))
    return os.path.join(py_dir, '__pycache__', '%s.%s' % (py_file, co.co_name))


def get_pydis_path(co):
    return get_save_prefix(co) + '.pydis'


def dump_pydis(co):
    with open(get_save_prefix(co) + '.pydis', 'wt') as f:
        disassemble_code(co, f)

def dump_binary(co, suffix, data):
    with open(get_save_prefix(co) + suffix, 'wb') as f:
        f.write(data)

def dump(co, ll, obj):
    assert os.path.isfile(co.co_filename)
    save_prefix = get_save_prefix(co)
    if obj is None:
        os.makedirs(os.path.dirname(save_prefix), exist_ok=True)
        with open(save_prefix + '.pydis', 'wt') as f:
            disassemble_code(co, f)
        with open(get_save_prefix(co) + '.ll', 'wb') as f:
            f.write(ll)
    else:
        with open(get_save_prefix(co) + '.opt.ll', 'wb') as f:
            f.write(ll)
        with open(save_prefix + '.o', 'wb') as f:
            f.write(obj)


def unpack_opargs(code):
    extended_arg = 0
    for i, (code_unit,) in enumerate(struct.iter_unpack('=H', code)):
        op = code_unit & 0xff
        short_arg = code_unit >> 8
        arg = short_arg | extended_arg
        extended_arg = (arg << 8) if op == EXTENDED_ARG else 0
        yield i, op, short_arg, arg


OPNAME_WIDTH = max(len(x) for x in opname)


def disassemble_code(co, file):
    print('%s @ %r\n' % (co.co_name, os.path.basename(co.co_filename)), file=file)
    lastline = None
    linestarts = {}
    for start, end, line in co.co_lines():
        if line is not None and line != lastline:
            lastline = linestarts[start] = line

    lineno_width = len(str(max(linestarts.values())))
    offset_width = len(str(len(co.co_code) - 2))
    line_format = f'%{lineno_width}s %2s %{offset_width}d %02x%02x    %-{OPNAME_WIDTH}s %s'

    labels = set()
    for offset, op, _, arg in unpack_opargs(co.co_code):
        if arg is not None:
            if op in hasjrel:
                labels.add(offset + 1 + arg)
            elif op in hasjabs:
                labels.add(arg)

    for offset, op, short_arg, arg in unpack_opargs(co.co_code):
        starts_line = linestarts.get(offset * 2, None)
        if starts_line is None:
            starts_line = ''
        else:
            starts_line = str(starts_line)

        if op < HAVE_ARGUMENT:
            arg_repr = ''
        elif op in haslocal:
            arg_repr = co.co_varnames[arg]
        elif op in hasconst:
            arg_repr = repr(co.co_consts[arg])
            if len(arg_repr) > 50:
                arg_repr = arg_repr[:50] + '...'
        elif op in hasname:
            arg_repr = co.co_names[arg]
        elif op in hasfree:
            ncells = len(co.co_cellvars)
            arg_repr = co.co_cellvars[arg] if arg < ncells else co.co_freevars[arg - ncells]
        elif op in hascompare:
            arg_repr = cmp_op[arg]
        elif op in hasjabs:
            arg_repr = '<to %d>' % arg
        elif op in hasjrel:
            arg_repr = '<to %d>' % (offset + 1 + arg)
        else:
            arg_repr = '<%d>' % arg

        print(line_format % (
            '' if starts_line is None else str(starts_line),
            '>>' if offset in labels else '',
            offset,
            op,
            short_arg,
            opname[op],
            arg_repr
        ), file=file)
