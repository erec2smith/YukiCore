#include <stdio.h>

int main(int argc, char **argv) {
    FILE *in, *out;
    int c, n = 0;
    if (argc != 3) {
        fprintf(stderr, "usage: embed <input.html> <output.h>\n");
        return 2;
    }
    in = fopen(argv[1], "rb");
    if (!in) {
        fprintf(stderr, "embed: cannot read %s\n", argv[1]);
        return 1;
    }
    out = fopen(argv[2], "wb");
    if (!out) {
        fclose(in);
        fprintf(stderr, "embed: cannot write %s\n", argv[2]);
        return 1;
    }
    fputs("static const unsigned char UI_HTML[] = {\n", out);
    while ((c = fgetc(in)) != EOF) {
        fprintf(out, "%d,", c);
        if (++n % 20 == 0) fputc('\n', out);
    }
    fputs("\n0\n};\n", out);
    fclose(in);
    fclose(out);
    return 0;
}
