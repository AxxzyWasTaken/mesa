/*
 * Copyright 2026 Aurora Silicon Project
 * SPDX-License-Identifier: MIT
 */

/*
 * Decode every committed G17G/G18G native witness text in
 * src/asahi/compiler/test/native/g18g through the in-tree generated decoder
 * for its encoding: *.g18g.bin under agx3d, *.g17g.bin under agx3c.
 *
 * Each text is `_agc.main.constant_program` at [0, 0x40) followed by
 * `_agc.main` at [0x40, size), as that directory's manifest.json records under
 * `symbols`. Both segments must
 *
 *  - decode with no unknown opcode,
 *  - be tiled exactly by the instruction lengths the decoder reports, and
 *  - end in `end`, ignoring trailing `nop` padding.
 *
 * Unknown *bits* inside a recognised form are not failures here: the XML does
 * not name every field yet, and those are reported, not guessed. The G18G
 * texts include the 24-byte smp form, the longest AGX3d encoding, so this is
 * also what exercises the decoder's full instruction width under ASan.
 */

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asahi/isa/disasm.h"
#include "util/bitset.h"
#include "util/macros.h"

#define CONSTANT_PROGRAM_SIZE 0x40

/* 128 shaders, each built for both architectures (manifest.json): the 41
 * oracle and instruction probes, the 16 control-flow kernels of
 * m5-control-flow-reference, the 32 compare/select kernels of
 * m5-compare-select-reference, m5-front-facing-reference's vertex and
 * fragment functions, the 30 kernels of m5-libagx-reference, and
 * m5-gl4-reference's three kernels and two vertex/fragment pairs. */
#define EXPECTED_TEXTS_PER_ARCH 128

typedef signed (*disassemble_instr_fn)(BITSET_WORD *, FILE *, unsigned,
                                       size_t, bool);

struct arch {
   const char *suffix;
   const char *decoder;
   disassemble_instr_fn disassemble;
   unsigned texts;
   unsigned instrs;
};

static bool
ends_with(const char *s, const char *suffix)
{
   size_t n = strlen(s), m = strlen(suffix);
   return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* The mnemonic is the first word of the last line; any "# XXX" diagnostics
 * for unknown bits come before it. */
static void
mnemonic(const char *text, char *out, size_t out_size)
{
   size_t len = strlen(text);
   while (len && text[len - 1] == '\n')
      len--;

   size_t start = len;
   while (start && text[start - 1] != '\n')
      start--;

   size_t n = 0;
   while (start + n < len && text[start + n] != ' ' && n + 1 < out_size) {
      out[n] = text[start + n];
      n++;
   }
   out[n] = 0;
}

static bool
check_segment(struct arch *arch, const char *name, const uint8_t *code,
              size_t start, size_t end)
{
   char last[32] = "";
   size_t i = start;

   while (i < end) {
      size_t available = end - i;
      char *text = NULL;
      size_t text_size = 0;

      FILE *fp = open_memstream(&text, &text_size);
      if (fp == NULL) {
         fprintf(stderr, "open_memstream failed\n");
         return false;
      }

      /* Exactly the bytes left, in a fresh allocation: word-aligned for the
       * BITSET_WORD interface, and bounded so a sanitizer sees any read past
       * the end of the segment. */
      uint8_t *copy = malloc(available);
      memcpy(copy, code + i, available);

      signed ret =
         arch->disassemble((BITSET_WORD *)copy, fp, i, available, false);
      fclose(fp);
      free(copy);

      unsigned length = abs(ret);
      bool unknown = strstr(text, "<unknown instr>") != NULL;
      char op[32];
      mnemonic(text, op, sizeof(op));
      free(text);

      if (unknown) {
         fprintf(stderr, "%s: %s: unknown opcode at 0x%zx\n", name,
                 arch->decoder, i);
         return false;
      }

      if (length == 0 || length > available) {
         fprintf(stderr, "%s: %s: length %u at 0x%zx overruns [0x%zx, 0x%zx)\n",
                 name, arch->decoder, length, i, start, end);
         return false;
      }

      if (strcmp(op, "nop") != 0)
         snprintf(last, sizeof(last), "%s", op);

      arch->instrs++;
      i += length;
   }

   if (strcmp(last, "end") != 0) {
      fprintf(stderr, "%s: %s: [0x%zx, 0x%zx) ends in '%s', not 'end'\n", name,
              arch->decoder, start, end, last);
      return false;
   }

   return true;
}

static bool
check_text(struct arch *arch, const char *dir, const char *name)
{
   char path[4096];
   snprintf(path, sizeof(path), "%s/%s", dir, name);

   FILE *f = fopen(path, "rb");
   if (f == NULL) {
      fprintf(stderr, "%s: cannot open\n", path);
      return false;
   }

   static uint8_t code[1 << 16];
   size_t size = fread(code, 1, sizeof(code), f);
   bool truncated = !feof(f);
   fclose(f);

   if (truncated || size <= CONSTANT_PROGRAM_SIZE) {
      fprintf(stderr, "%s: unexpected size %zu\n", path, size);
      return false;
   }

   /* Check both segments even when the first fails. */
   bool pass = check_segment(arch, name, code, 0, CONSTANT_PROGRAM_SIZE);
   pass &= check_segment(arch, name, code, CONSTANT_PROGRAM_SIZE, size);
   return pass;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <native/g18g corpus directory>\n", argv[0]);
      return 1;
   }

   struct arch arches[] = {
      {.suffix = ".g18g.bin", .decoder = "agx3d",
       .disassemble = agx3d_disassemble_instr},
      {.suffix = ".g17g.bin", .decoder = "agx3c",
       .disassemble = agx3c_disassemble_instr},
   };

   DIR *dir = opendir(argv[1]);
   if (dir == NULL) {
      fprintf(stderr, "%s: cannot open directory\n", argv[1]);
      return 1;
   }

   bool pass = true;
   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL) {
      for (unsigned i = 0; i < ARRAY_SIZE(arches); ++i) {
         if (ends_with(entry->d_name, arches[i].suffix)) {
            arches[i].texts++;
            pass &= check_text(&arches[i], argv[1], entry->d_name);
         }
      }
   }
   closedir(dir);

   for (unsigned i = 0; i < ARRAY_SIZE(arches); ++i) {
      if (arches[i].texts != EXPECTED_TEXTS_PER_ARCH) {
         fprintf(stderr, "%s: found %u texts, expected %u\n",
                 arches[i].suffix, arches[i].texts, EXPECTED_TEXTS_PER_ARCH);
         pass = false;
      }

      printf("%s: %u texts, %u instructions under %s\n", arches[i].suffix,
             arches[i].texts, arches[i].instrs, arches[i].decoder);
   }

   printf("%s\n", pass ? "Pass." : "FAIL");
   return pass ? 0 : 1;
}
