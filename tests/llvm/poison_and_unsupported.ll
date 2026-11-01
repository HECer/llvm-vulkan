; LV-PASSES: lv-fold
; LV-EXPECT: %keep_nsw = add nsw i8 127, 1
; LV-EXPECT: %keep_nuw = add nuw i8 -1, 1
; LV-EXPECT: %keep_exact = lshr exact i32 3, 1
; LV-EXPECT: %keep_zero = udiv i32 7, 0
; LV-EXPECT: %keep_overflow = sdiv i32 -2147483648, -1
; LV-EXPECT: %keep_shift = shl i32 1, 32
; LV-EXPECT: %keep_small_shift = lshr i3 -1, 3
; LV-EXPECT: %keep_undef = add i32 undef, 1
; LV-EXPECT: %keep_poison = xor i32 poison, 1
; LV-EXPECT: %keep_wide = add i128 18446744073709551616, 7
; LV-EXPECT: %keep_vector = add <2 x i32>
; LV-EXPECT: %keep_fp = fadd double
; LV-EXPECT: %keep_frozen = freeze i32 poison
; LV-EXPECT: %keep_disjoint = or disjoint i32 1, 1
; LV-EXPECT: %keep_srem_overflow = srem i32 -2147483648, -1

define i8 @nsw() {
  %keep_nsw = add nsw i8 127, 1
  ret i8 %keep_nsw
}
define i8 @nuw() {
  %keep_nuw = add nuw i8 -1, 1
  ret i8 %keep_nuw
}
define i32 @exact() {
  %keep_exact = lshr exact i32 3, 1
  ret i32 %keep_exact
}
define i32 @div_zero() {
  %keep_zero = udiv i32 7, 0
  ret i32 %keep_zero
}
define i32 @div_overflow() {
  %keep_overflow = sdiv i32 -2147483648, -1
  ret i32 %keep_overflow
}
define i32 @big_shift() {
  %keep_shift = shl i32 1, 32
  ret i32 %keep_shift
}
define i3 @small_shift() {
  %keep_small_shift = lshr i3 7, 3
  ret i3 %keep_small_shift
}
define i32 @undefined() {
  %keep_undef = add i32 undef, 1
  ret i32 %keep_undef
}
define i32 @poison_input() {
  %keep_poison = xor i32 poison, 1
  ret i32 %keep_poison
}
define i128 @wide() {
  %keep_wide = add i128 18446744073709551616, 7
  ret i128 %keep_wide
}
define <2 x i32> @vector_input() {
  %keep_vector = add <2 x i32> <i32 1, i32 2>, <i32 3, i32 4>
  ret <2 x i32> %keep_vector
}
define double @floating() {
  %keep_fp = fadd double 1.0, 2.0
  ret double %keep_fp
}
define i32 @freeze_poison() {
  %keep_frozen = freeze i32 poison
  ret i32 %keep_frozen
}
define i32 @disjoint_overlap() {
  %keep_disjoint = or disjoint i32 1, 1
  ret i32 %keep_disjoint
}
define i32 @remainder_overflow() {
  %keep_srem_overflow = srem i32 -2147483648, -1
  ret i32 %keep_srem_overflow
}
