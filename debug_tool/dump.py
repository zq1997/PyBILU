import os


def dump(co, ll, opt_ll, obj):
    assert os.path.isfile(co.co_filename)

    with io.StringIO() as output:
        _disassemble_bytes(co, output)
        print(output.getvalue())

    save_name = '%s.%d.%s' % (co.co_filename, co.co_firstlineno, co.co_name)
    with open(save_name + '.ll', 'wb') as f:
        f.write(ll)
    with open(save_name + '.opt.ll', 'wb') as f:
        f.write(opt_ll)
    with open(save_name + '.o', 'wb') as f:
        f.write(obj)


import types
import collections
import io

from opcode import *
from opcode import __all__ as _opcodes_all

__all__ = ["code_info", "dis", "disassemble", "distb", "disco",
           "findlinestarts", "findlabels", "show_code",
           "get_instructions", "Instruction", "Bytecode"] + _opcodes_all
del _opcodes_all

_have_code = (types.MethodType, types.FunctionType, types.CodeType,
              classmethod, staticmethod, type)

FORMAT_VALUE = opmap['FORMAT_VALUE']
FORMAT_VALUE_CONVERTERS = (
    (None, ''),
    (str, 'str'),
    (repr, 'repr'),
    (ascii, 'ascii'),
)
MAKE_FUNCTION = opmap['MAKE_FUNCTION']
MAKE_FUNCTION_FLAGS = ('defaults', 'kwdefaults', 'annotations', 'closure')

_Instruction = collections.namedtuple("_Instruction",
                                      "opname opcode arg argval argrepr offset starts_line is_jump_target")

_OPNAME_WIDTH = 20
_OPARG_WIDTH = 5


class Instruction(_Instruction):
    """Details for a bytecode operation

       Defined fields:
         opname - human readable name for operation
         opcode - numeric code for operation
         arg - numeric argument to operation (if any), otherwise None
         argval - resolved arg value (if known), otherwise same as arg
         argrepr - human readable description of operation argument
         offset - start index of operation within bytecode sequence
         starts_line - line started by this opcode (if any), otherwise None
         is_jump_target - True if other code jumps to here, otherwise False
    """

    def _disassemble(self, lineno_width=3, mark_as_current=False, offset_width=4):
        """Format instruction details for inclusion in disassembly output

        *lineno_width* sets the width of the line number field (0 omits it)
        *mark_as_current* inserts a '-->' marker arrow as part of the line
        *offset_width* sets the width of the instruction offset field
        """
        fields = []
        # Column: Source code line number
        if lineno_width:
            if self.starts_line is not None:
                lineno_fmt = "%%%dd" % lineno_width
                fields.append(lineno_fmt % self.starts_line)
            else:
                fields.append(' ' * lineno_width)
        # Column: Current instruction indicator
        if mark_as_current:
            fields.append('-->')
        else:
            fields.append('   ')
        # Column: Jump target marker
        if self.is_jump_target:
            fields.append('>>')
        else:
            fields.append('  ')
        # Column: Instruction offset from start of code sequence
        fields.append(repr(self.offset).rjust(offset_width))
        # Column: Opcode name
        fields.append(self.opname.ljust(_OPNAME_WIDTH))
        # Column: Opcode argument
        if self.arg is not None:
            fields.append(repr(self.arg).rjust(_OPARG_WIDTH))
            # Column: Opcode argument details
            if self.argrepr:
                fields.append('(' + self.argrepr + ')')
        return ' '.join(fields).rstrip()


def _get_const_info(const_index, const_list):
    """Helper to get optional details about const references

       Returns the dereferenced constant and its repr if the constant
       list is defined.
       Otherwise returns the constant index and its repr().
    """
    argval = const_index
    if const_list is not None:
        argval = const_list[const_index]
    return argval, repr(argval)


def _get_name_info(name_index, name_list):
    """Helper to get optional details about named references

       Returns the dereferenced name as both value and repr if the name
       list is defined.
       Otherwise returns the name index and its repr().
    """
    argval = name_index
    if name_list is not None:
        argval = name_list[name_index]
        argrepr = argval
    else:
        argrepr = repr(argval)
    return argval, argrepr


def _get_instructions_bytes(code, varnames=None, names=None, constants=None,
                            cells=None, linestarts=None):
    """Iterate over the instructions in a bytecode string.

    Generates a sequence of Instruction namedtuples giving the details of each
    opcode.  Additional information about the code's runtime environment
    (e.g. variable names, constants) can be specified using optional
    arguments.

    """
    labels = findlabels(code)
    starts_line = None
    for offset, op, arg in _unpack_opargs(code):
        if linestarts is not None:
            starts_line = linestarts.get(offset, None)
        is_jump_target = offset in labels
        argval = None
        argrepr = ''
        if arg is not None:
            #  Set argval to the dereferenced value of the argument when
            #  available, and argrepr to the string representation of argval.
            #    _disassemble_bytes needs the string repr of the
            #    raw name index for LOAD_GLOBAL, LOAD_CONST, etc.
            argval = arg
            if op in hasconst:
                argval, argrepr = _get_const_info(arg, constants)
            elif op in hasname:
                argval, argrepr = _get_name_info(arg, names)
            elif op in hasjabs:
                argval = arg * 2
                argrepr = "to " + repr(argval)
            elif op in hasjrel:
                argval = offset + 2 + arg * 2
                argrepr = "to " + repr(argval)
            elif op in haslocal:
                argval, argrepr = _get_name_info(arg, varnames)
            elif op in hascompare:
                argval = cmp_op[arg]
                argrepr = argval
            elif op in hasfree:
                argval, argrepr = _get_name_info(arg, cells)
            elif op == FORMAT_VALUE:
                argval, argrepr = FORMAT_VALUE_CONVERTERS[arg & 0x3]
                argval = (argval, bool(arg & 0x4))
                if argval[1]:
                    if argrepr:
                        argrepr += ', '
                    argrepr += 'with format'
            elif op == MAKE_FUNCTION:
                argrepr = ', '.join(s for i, s in enumerate(MAKE_FUNCTION_FLAGS)
                                    if arg & (1 << i))
        yield Instruction(opname[op], op,
                          arg, argval, argrepr,
                          offset, starts_line, is_jump_target)


def _disassemble_bytes(co, file):
    code = co.co_code
    lasti = -1
    varnames = co.co_varnames
    names = co.co_names
    constants = co.co_consts
    cells = co.co_cellvars + co.co_freevars
    linestarts = dict(findlinestarts(co))
    # Omit the line number column entirely if we have no line number info
    maxlineno = max(linestarts.values())
    lineno_width = len(str(maxlineno))
    maxoffset = len(code) - 2
    offset_width = len(str(maxoffset))
    for instr in _get_instructions_bytes(code, varnames, names, constants, cells, linestarts):
        new_source_line = instr.starts_line is not None and instr.offset > 0
        if new_source_line:
            print(file=file)
        is_current_instr = instr.offset == lasti
        print(instr._disassemble(lineno_width, is_current_instr, offset_width),
              file=file)


def _unpack_opargs(code):
    extended_arg = 0
    for i in range(0, len(code), 2):
        op = code[i]
        if op >= HAVE_ARGUMENT:
            arg = code[i + 1] | extended_arg
            extended_arg = (arg << 8) if op == EXTENDED_ARG else 0
        else:
            arg = None
            extended_arg = 0
        yield (i, op, arg)


def findlabels(code):
    """Detect all offsets in a byte code which are jump targets.

    Return the list of offsets.

    """
    labels = []
    for offset, op, arg in _unpack_opargs(code):
        if arg is not None:
            if op in hasjrel:
                label = offset + 2 + arg * 2
            elif op in hasjabs:
                label = arg * 2
            else:
                continue
            if label not in labels:
                labels.append(label)
    return labels


def findlinestarts(code):
    """Find the offsets in a byte code which are start of lines in the source.

    Generate pairs (offset, lineno)
    """
    lastline = None
    for start, end, line in code.co_lines():
        if line is not None and line != lastline:
            lastline = line
            yield start, line
    return
