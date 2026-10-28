# CIE2026 AsyncSys — 三档单档 + 合一面积综合（SMIC 0.18um，典型角，compile_ultra）
set libdir "/mnt/shared/pdk/SMIC/0.18um/db"
set search_path ". $libdir"
set target_library "scc018ug_uhd_rvt_tt_v1p8_25c_ccs.db"
set link_library "* $target_library"
set hdlin_enable_v2001_naming true

proc syn_file {svfile rpt} {
  analyze -format sverilog -define SYNTHESIS [list $svfile]
  elaborate VDot64b
  current_design VDot64b
  link
  create_clock -name clk -period 2.0 [get_ports clock]
  set_clock_uncertainty 0.1 [get_clocks clk]
  set_false_path -from [get_ports reset]
  compile_ultra -no_autoungroup
  report_area -hierarchy > $rpt.area.rpt
  report_timing -delay max -max_paths 3 > $rpt.timing.rpt
  puts "DONE_$rpt"
  remove_design -all
}

syn_file VDot_int8.sv  int8
syn_file VDot_int4.sv  int4
syn_file VDot_int16.sv int16
syn_file VDot64b_all.sv all
puts "ALL_SYN2_DONE"
