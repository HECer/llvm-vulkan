; LV-PASSES: lv-fold
; LV-EXPECT: ret i1 false
; LV-EXPECT: ret i8 -1
; LV-EXPECT: ret i8 127
; LV-EXPECT: ret i8 -2
; LV-EXPECT: ret i3 -2
; LV-EXPECT: ret i32 -2147483648
; LV-EXPECT: ret i32 2147483647
; LV-EXPECT: ret i32 -1
; LV-EXPECT: ret i31 -1073741824
; LV-EXPECT: ret i1 true
; LV-REJECT: %fold_
; LV-EXIT: 0

define i1 @i1_wrap() {
  %fold_a = add i1 true, true
  ret i1 %fold_a
}
define i8 @negative_remainder() {
  %fold_a = srem i8 -10, 3
  ret i8 %fold_a
}
define i8 @negative_divisor() {
  %fold_a = sdiv i8 10, -3
  %fold_b = add i8 %fold_a, 1
  ret i8 %fold_b
}
define i8 @unsigned_shift() {
  %fold_a = lshr i8 -1, 1
  ret i8 %fold_a
}
define i3 @odd_unsigned_division() {
  %fold_a = udiv i3 7, 1
  %fold_b = sub i3 %fold_a, 1
  ret i3 %fold_b
}
define i32 @top_bit() {
  %fold_a = shl i32 1, 31
  ret i32 %fold_a
}
define i32 @clear_sign_bit() {
  %fold_a = lshr i32 -1, 1
  ret i32 %fold_a
}
define i32 @sign_extend_shift() {
  %fold_a = ashr i32 -2147483648, 31
  ret i32 %fold_a
}
define i31 @odd_sign() {
  %fold_a = sub i31 0, 1073741824
  ret i31 %fold_a
}
define i1 @signed_min_less() {
  %fold_a = icmp slt i32 -2147483648, 2147483647
  ret i1 %fold_a
}
define i1 @unsigned_max_greater() {
  %fold_a = icmp ugt i32 -1, 2147483647
  ret i1 %fold_a
}

define i32 @main() {
  %a = call i32 @top_bit()
  %b = call i32 @sign_extend_shift()
  %c = call i8 @negative_remainder()
  %d = call i3 @odd_unsigned_division()
  %ca = icmp eq i32 %a, -2147483648
  %cb = icmp eq i32 %b, -1
  %cc = icmp eq i8 %c, -1
  %cd = icmp eq i3 %d, 6
  %ab = and i1 %ca, %cb
  %abc = and i1 %ab, %cc
  %ok = and i1 %abc, %cd
  %result = select i1 %ok, i32 0, i32 1
  ret i32 %result
}
