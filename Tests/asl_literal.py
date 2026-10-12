"""Restricted literal ASL package reader for source-backed tests, never AML execution."""
import re


def named_package(text, name):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    match = re.search(r"\bName\s*\(\s*" + re.escape(name) + r"\s*,\s*(Package\b)", text)
    if match is None:
        raise ValueError("Missing literal package " + name)
    start = match.start(1)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        if text[end] == "{": depth += 1
        if text[end] == "}": depth -= 1
        end += 1
    tokens = re.findall(r'"[^"\\]*"|0x[\da-fA-F]+|\b[A-Za-z_]\w*\b|\d+|[(){},]', text[start:end])
    index = 0

    def consume(token):
        nonlocal index
        assert tokens[index] == token, (name, index, tokens[index], token)
        index += 1

    def value():
        nonlocal index
        token = tokens[index]
        index += 1
        if token == "Package":
            consume("(")
            count = None
            if tokens[index] != ")":
                count = int(tokens[index], 0)
                index += 1
            consume(")"); consume("{")
            values = []
            while tokens[index] != "}":
                values.append(value())
                if tokens[index] == "}": break
                consume(",")
            consume("}")
            assert count is None or count == len(values)
            return values
        if token.startswith('"'): return token[1:-1]
        if token == "Zero": return 0
        if token == "One": return 1
        return int(token, 0)

    tree = value()
    assert index == len(tokens)
    return tree


def c_fixture(named_trees):
    definitions = []

    def emit(value):
        if isinstance(value, list):
            children = [emit(child) for child in value]
            symbol = "AcpiNodes" + str(len(definitions))
            if children:
                definitions.append("static const CR_PEP_VALUE " + symbol + "[] = {" +
                                   ",\n".join(children) + "};")
                return '{.Type=CrPepPackage,.Package={' + symbol + ',' + str(len(children)) + '}}'
            return '{.Type=CrPepPackage,.Package={NULL,0}}'
        if isinstance(value, int): return '{.Type=CrPepInteger,.Integer=' + str(value) + 'ULL}'
        raise ValueError("Only integer GPIO fixtures are supported")

    for name, tree in named_trees.items():
        root = emit(tree)
        definitions.append("static const CR_PEP_VALUE " + name + " = " + root + ";")
    return "\n".join(definitions) + "\n"
