; LV-PASSES: lv-cfg
; LV-EXPECT: indirectbr ptr %target
; LV-EXPECT: first_target:
; LV-EXPECT: second_target:
; LV-EXPECT: address_only:
; LV-REJECT: detached_cycle_a:
; LV-REJECT: detached_cycle_b:
; LV-REJECT: dead_switch:
; LV-EXIT: 0

@targets = global [2 x ptr] [ptr blockaddress(@dispatch, %first_target), ptr blockaddress(@dispatch, %second_target)]
@nested = global {i32, [1 x ptr]} {i32 7, [1 x ptr] [ptr blockaddress(@dispatch, %address_only)]}

define i32 @dispatch(i32 %index) {
entry:
  %slot = getelementptr [2 x ptr], ptr @targets, i32 0, i32 %index
  %target = load ptr, ptr %slot
  indirectbr ptr %target, [label %first_target, label %second_target]
first_target:
  br label %join
second_target:
  br label %join
address_only:
  br label %join
dead_switch:
  switch i32 9, label %join [i32 1, label %join]
join:
  %r = phi i32 [10, %first_target], [20, %second_target], [30, %address_only], [40, %dead_switch], [40, %dead_switch]
  ret i32 %r
detached_cycle_a:
  br label %detached_cycle_b
detached_cycle_b:
  br label %detached_cycle_a
}

define i32 @main() {
  %a = call i32 @dispatch(i32 0)
  %b = call i32 @dispatch(i32 1)
  %sum = add i32 %a, %b
  %ok = icmp eq i32 %sum, 30
  %r = select i1 %ok, i32 0, i32 1
  ret i32 %r
}
