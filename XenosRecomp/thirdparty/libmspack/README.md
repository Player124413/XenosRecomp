# libmspack (LZX decoder)

This directory contains the files of [libmspack](https://github.com/kyz/libmspack) that are
needed to decode the LZX compressed shader archives of Xbox 360 games, taken from commit
`55d501976171397ccd5d5a7a1ca7da065b1d9a06`.

Only the LZX decoder (`lzxd.c`) and the headers it needs are included; the compression support
for the other formats of the library (`cab`, `chm`, `hlp`, `kwaj`, `lit`, `mszip`, ...) is left
out. The decoder is used through `xcompress.cpp`, which implements the container format that
`XMemCompress`/`XMemDecompress` write on the Xbox 360.

libmspack is licensed under the GNU Lesser General Public License version 2.1; see
`COPYING.LIB`. The files in this directory are unmodified.
