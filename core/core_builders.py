"""Functions used to generate source files during build time

All such functions are invoked in a subprocess on Windows to prevent build flakiness.
"""
import zlib

from platform_methods import subprocess_main


def escape_string(s):
    def charcode_to_c_escapes(c):
        rev_result = []
        while c >= 256:
            c, low = (c // 256, c % 256)
            rev_result.append("\\%03o" % low)
        rev_result.append("\\%03o" % c)
        return "".join(reversed(rev_result))

    result = ""
    if isinstance(s, str):
        s = s.encode("utf-8")
    for c in s:
        if not (32 <= c < 127) or c in (ord("\\"), ord('"')):
            result += charcode_to_c_escapes(c)
        else:
            result += chr(c)
    return result


def make_certs_header(target, source, env):
    src = source[0]
    dst = target[0]
    f = open(src, "rb")
    g = open(dst, "w", encoding="utf-8")
    buf = f.read()
    decomp_size = len(buf)

    # Use maximum zlib compression level to further reduce file size
    # (at the cost of initial build times).
    buf = zlib.compress(buf, zlib.Z_BEST_COMPRESSION)

    g.write("/* THIS FILE IS GENERATED DO NOT EDIT */\n")
    g.write("#ifndef CERTS_COMPRESSED_GEN_H\n")
    g.write("#define CERTS_COMPRESSED_GEN_H\n")

    # System certs path. Editor will use them if defined. (for package maintainers)
    path = env["system_certs_path"]
    g.write('#define _SYSTEM_CERTS_PATH "%s"\n' % str(path))
    if env["builtin_certs"]:
        # Defined here and not in env so changing it does not trigger a full rebuild.
        g.write("#define BUILTIN_CERTS_ENABLED\n")
        g.write("static const int _certs_compressed_size = " + str(len(buf)) + ";\n")
        g.write("static const int _certs_uncompressed_size = " + str(decomp_size) + ";\n")
        g.write("static const unsigned char _certs_compressed[] = {\n")
        for i in range(len(buf)):
            g.write("\t" + str(buf[i]) + ",\n")
        g.write("};\n")
    g.write("#endif // CERTS_COMPRESSED_GEN_H")

    g.close()
    f.close()

if __name__ == "__main__":
    subprocess_main(globals())
