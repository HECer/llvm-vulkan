; LV-PASSES: lv-fold
; LV-EXPECT: ret i64 0
; LV-EXPECT: ret i64 8589934593
; LV-EXPECT: ret i64 -3074457345618258602
; LV-EXPECT: ret i64 -1
; LV-EXPECT: ret i64 9223372036854775807
; LV-EXPECT: ret i33 0
; LV-EXPECT: ret i48 -140737488355328
; LV-EXPECT: ret i1 true
; LV-EXPECT: ret i1 false
; LV-REJECT: %fold_
; LV-EXIT: 0

define i64 @wrap64() {
  %fold_a = add i64 -1, 1
  ret i64 %fold_a
}
define i64 @multiply64() {
  %fold_a = mul i64 4294967297, 4294967297
  ret i64 %fold_a
}
define i64 @signed_div64() {
  %fold_a = sdiv i64 -9223372036854775807, 3
  ret i64 %fold_a
}
define i64 @signed_rem64() {
  %fold_a = srem i64 -9223372036854775807, 3
  ret i64 %fold_a
}
define i64 @arithmetic_shift64() {
  %fold_a = ashr i64 -9223372036854775808, 63
  ret i64 %fold_a
}
define i64 @logical_shift64() {
  %fold_a = lshr i64 -1, 1
  ret i64 %fold_a
}
define i33 @odd_width33() {
  %fold_a = add i33 8589934591, 1
  ret i33 %fold_a
}
define i48 @odd_sign48() {
  %fold_a = sub i48 0, 140737488355328
  ret i48 %fold_a
}
define i1 @comparison64() {
  %fold_a = icmp ult i64 9223372036854775807, -1
  ret i1 %fold_a
}
define i1 @mixed_frontiers() {
  %fold_wide = icmp ult i64 9223372036854775807, -1
  %fold_narrow = icmp ult i8 1, 2
  %fold_join = xor i1 %fold_wide, %fold_narrow
  ret i1 %fold_join
}
define i32 @main() {
  %a = call i64 @multiply64()
  %b = call i64 @signed_div64()
  %c = call i64 @signed_rem64()
  %d = call i1 @comparison64()
  %e = call i1 @mixed_frontiers()
  %ca = icmp eq i64 %a, 8589934593
  %cb = icmp eq i64 %b, -3074457345618258602
  %cc = icmp eq i64 %c, -1
  %ab = and i1 %ca, %cb
  %abc = and i1 %ab, %cc
  %abcd = and i1 %abc, %d
  %ce = xor i1 %e, true
  %ok = and i1 %abcd, %ce
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
