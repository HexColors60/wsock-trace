## Generating documentation

The documentation is generated by **[Doxygen](http://www.stack.nl/~dimitri/doxygen/index.html)**
via the `Wsock-Trace.Doxyfile` config-file.

Running the command `doxygen Wsock-Trace.Doxyfile`, requires `hhc.exe`.
This is part of the **[Microsoft HTML Workshop](https://www.microsoft.com/en-us/download/details.aspx?id=21138)**.
This will generate the compressed HTML-file `Wsock-Trace.chm` of the generated documentation.

`Wsock-Trace.chm` can be view by the command `start Wsock-Trace.chm` (which is normally
pointing to `"%WINDIR%\hh.exe"`). Or loaded in the excellent
**[KchmViewer](http://www.ulduzsoft.com/linux/kchmviewer/getting-kchmviewer/)**.

But it's much easier using the GNU `Makefile` directly. Enter this directory
and issue `make` or `mingw32-make` with one if these targets:
```
make all   - should produce Wsock-Trace.chm in this directory.
make clean - should delete all generated files.
```

*PS1*. When using the `Index` menu in `"%WINDIR%\hh.exe"`, it will crash!
  Some issue with how I or Doxygen creates the `output/index.hhk` file?
   **[KchmViewer](http://www.ulduzsoft.com/linux/kchmviewer/getting-kchmviewer/)** does
   not exhibit this bug.

*PS2*. This file is written with the aid of the **[Atom](https://atom.io/)**
   editor and it's **[Markdown-Preview](https://atom.io/packages/markdown-preview)**.
   A real time-saver.