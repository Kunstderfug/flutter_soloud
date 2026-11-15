import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:typed_data';

/// Reads the NUL-terminated UTF-8 text of [buffer] (at most [capacity]
/// bytes). Malformed UTF-8, such as a multi-byte character cut off by the
/// native side's truncation, decodes as U+FFFD instead of throwing.
String readClapText(ffi.Pointer<ffi.Char> buffer, int capacity) {
  if (buffer == ffi.nullptr || capacity <= 0) {
    return '';
  }
  final bytes = buffer.cast<ffi.Uint8>().asTypedList(capacity);
  var length = bytes.indexOf(0);
  if (length < 0) {
    length = capacity;
  }
  return utf8.decode(
    Uint8List.sublistView(bytes, 0, length),
    allowMalformed: true,
  );
}
