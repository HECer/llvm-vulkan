; LV-PASSES: lv-cse
; LV-EXPECT: %byte_offset = getelementptr i8, ptr %base, i32 1
; LV-EXPECT: %word_offset = getelementptr i32, ptr %base, i32 1
; LV-EXPECT: %equal = icmp eq i32 %x, %y
; LV-EXPECT: %less = icmp slt i32 %x, %y
; LV-EXPECT: %first_field = extractvalue { i32, i32 } %pair, 0
; LV-EXPECT: %second_field = extractvalue { i32, i32 } %pair, 1
; LV-EXPECT: %identity = shufflevector <2 x i32> %vec, <2 x i32> poison, <2 x i32> <i32 0, i32 1>
; LV-EXPECT: %reverse = shufflevector <2 x i32> %vec, <2 x i32> poison, <2 x i32> <i32 1, i32 0>
; LV-EXIT: 0

; These pairs have identical serialized candidate records, deliberately forcing
; collisions. LLVM's exact comparison must check non-operand semantic state:
; GEP source element type, comparison predicate, aggregate indices, shuffle mask.
@bytes = global [8 x i8] zeroinitializer

define i1 @different_gep(ptr %base) {
  %byte_offset = getelementptr i8, ptr %base, i32 1
  %word_offset = getelementptr i32, ptr %base, i32 1
  %different = icmp ne ptr %byte_offset, %word_offset
  ret i1 %different
}

define i1 @different_predicate(i32 %x, i32 %y) {
  %equal = icmp eq i32 %x, %y
  %less = icmp slt i32 %x, %y
  %different = xor i1 %equal, %less
  ret i1 %different
}

define i1 @different_index({i32, i32} %pair) {
  %first_field = extractvalue {i32, i32} %pair, 0
  %second_field = extractvalue {i32, i32} %pair, 1
  %different = icmp ne i32 %first_field, %second_field
  ret i1 %different
}

define i1 @different_mask(<2 x i32> %vec) {
  %identity = shufflevector <2 x i32> %vec, <2 x i32> poison, <2 x i32> <i32 0, i32 1>
  %reverse = shufflevector <2 x i32> %vec, <2 x i32> poison, <2 x i32> <i32 1, i32 0>
  %a = extractelement <2 x i32> %identity, i32 0
  %b = extractelement <2 x i32> %reverse, i32 0
  %different = icmp ne i32 %a, %b
  ret i1 %different
}

define i32 @main() {
  %a = call i1 @different_gep(ptr @bytes)
  %b = call i1 @different_predicate(i32 1, i32 2)
  %c = call i1 @different_index({i32, i32} {i32 7, i32 9})
  %d = call i1 @different_mask(<2 x i32> <i32 11, i32 13>)
  %ab = and i1 %a, %b
  %cd = and i1 %c, %d
  %ok = and i1 %ab, %cd
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
