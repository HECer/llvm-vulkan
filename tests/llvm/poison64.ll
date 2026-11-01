; LV-PASSES: lv-fold
; LV-EXPECT: %keep_nsw = add nsw i64 9223372036854775807, 1
; LV-EXPECT: %keep_nuw = add nuw i64 -1, 1
; LV-EXPECT: %keep_exact = lshr exact i64 3, 1
; LV-EXPECT: %keep_div = sdiv i64 -9223372036854775808, -1
; LV-EXPECT: %keep_rem = srem i64 -9223372036854775808, -1
; LV-EXPECT: %keep_shift = shl i64 1, 64
; LV-EXPECT: %keep_zero = udiv i64 7, 0

define i64 @overflow_signed64() {
  %keep_nsw = add nsw i64 9223372036854775807, 1
  ret i64 %keep_nsw
}
define i64 @overflow_unsigned64() {
  %keep_nuw = add nuw i64 -1, 1
  ret i64 %keep_nuw
}
define i64 @inexact64() {
  %keep_exact = lshr exact i64 3, 1
  ret i64 %keep_exact
}
define i64 @divide_overflow64() {
  %keep_div = sdiv i64 -9223372036854775808, -1
  ret i64 %keep_div
}
define i64 @remainder_overflow64() {
  %keep_rem = srem i64 -9223372036854775808, -1
  ret i64 %keep_rem
}
define i64 @overshift64() {
  %keep_shift = shl i64 1, 64
  ret i64 %keep_shift
}
define i64 @divide_zero64() {
  %keep_zero = udiv i64 7, 0
  ret i64 %keep_zero
}
