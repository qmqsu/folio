PDFio 1.7.0, https://github.com/michaelrsweet/pdfio, commit f5841b573f950dede2783f1c5d73101d2d132977. See LICENSE and NOTICE. The embedded reader omits the system font cache and content creation module, and uses the project miniz on ESP.

The read-only port omits writer finalization, bounds inherited page-resource traversal, allocates the recursive array tokenizer on the heap, and accounts for 32-bit file offsets. The PDF parser runs on a dedicated PSRAM stack sized for the upstream cross-reference scratch arrays. All embedded miniz exports use a private namespace.
