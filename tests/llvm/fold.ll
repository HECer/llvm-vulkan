; LV-PASSES: lv-fold
; LV-EXPECT: ret i32 42
; LV-EXPECT: ret i8 -128
; LV-EXPECT: ret i3 0
; LV-EXPECT: ret i1 true
; LV-EXPECT: ret i32 -3
; LV-REJECT: %fold_
; LV-EXIT: 0

define i32 @chain() {
entry:
  %fold_a = add i32 19, 23
  %fold_b = mul i32 %fold_a, 9
  %fold_c = udiv i32 %fold_b, 9
  ret i32 %fold_c
}

define i8 @wrap() {
  %fold_a = add i8 127, 1
  ret i8 %fold_a
}

define i3 @odd_width() {
  %fold_a = add i3 7, 1
  ret i3 %fold_a
}

define i1 @signed_compare() {
  %fold_a = icmp slt i8 -1, 1
  ret i1 %fold_a
}

define i32 @signed_divide() {
  %fold_a = sdiv i32 -10, 3
  ret i32 %fold_a
}

define i32 @main() {
  %a = call i32 @chain()
  %b = call i8 @wrap()
  %c = call i3 @odd_width()
  %d = call i1 @signed_compare()
  %e = call i32 @signed_divide()
  %ca = icmp eq i32 %a, 42
  %cb = icmp eq i8 %b, -128
  %cc = icmp eq i3 %c, 0
  %ce = icmp eq i32 %e, -3
  %ab = and i1 %ca, %cb
  %abc = and i1 %ab, %cc
  %abcd = and i1 %abc, %d
  %ok = and i1 %abcd, %ce
  %result = select i1 %ok, i32 0, i32 1
  ret i32 %result
}
