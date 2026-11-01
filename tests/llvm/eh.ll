; LV-PASSES: lv-pipeline
; LV-EXPECT: invoke void @may_throw()
; LV-EXPECT: landingpad { ptr, i32 }
; LV-EXPECT: resume { ptr, i32 } %landing
; LV-REJECT: %dead_math

declare void @may_throw()
declare i32 @__gxx_personality_v0(...)

define void @exception_path() personality ptr @__gxx_personality_v0 {
entry:
  %dead_math = add i32 17, 19
  invoke void @may_throw() to label %normal unwind label %cleanup
normal:
  ret void
cleanup:
  %landing = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %landing
}
