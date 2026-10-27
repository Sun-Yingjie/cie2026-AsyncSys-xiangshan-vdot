set libdir "/mnt/shared/pdk/SMIC/0.18um/db"
set search_path ". $libdir"
set target_library "scc018ug_uhd_rvt_tt_v1p8_25c_ccs.db"
set link_library "* $target_library"
analyze -format sverilog {mul8.sv}
elaborate mul8
current_design mul8
link
create_clock -name clk -period 2.0 [get_ports a*]
compile
report_area > mul8_area.rpt
puts MUL8_DONE
