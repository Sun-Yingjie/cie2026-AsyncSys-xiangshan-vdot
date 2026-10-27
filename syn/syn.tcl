# CIE2026 AsyncSys — VDot64b 面积综合（SMIC 0.18um，典型角）
set libdir "/mnt/shared/pdk/SMIC/0.18um/db"
set search_path ". $libdir"
set target_library "scc018ug_uhd_rvt_tt_v1p8_25c_ccs.db"
set link_library "* $target_library"
set hdlin_enable_v2001_naming true

analyze -format sverilog -define SYNTHESIS {VDot64b.sv}
elaborate VDot64b
current_design VDot64b
link

create_clock -name clk -period 2.0 [get_ports clock]
set_clock_uncertainty 0.1 [get_clocks clk]
set_false_path -from [get_ports reset]

compile_ultra -no_autoungroup

report_area -hierarchy > area.rpt
report_timing -delay max -max_paths 5 > timing.rpt
report_reference > reference.rpt
puts "SYN_DONE"
