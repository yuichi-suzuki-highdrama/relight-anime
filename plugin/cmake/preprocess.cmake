# cl /EP で前処理して OUT に書き出す (PiPL 生成用)。
# 引数: -DCL=<cl.exe> -DIN=<入力> -DOUT=<出力> [-DINCLUDES=a;b] [-DDEFINES=X;Y]
set(args /nologo)
foreach(inc IN LISTS INCLUDES)
  list(APPEND args "/I${inc}")
endforeach()
foreach(def IN LISTS DEFINES)
  list(APPEND args "/D${def}")
endforeach()
list(APPEND args /EP "${IN}")
execute_process(
  COMMAND "${CL}" ${args}
  OUTPUT_FILE "${OUT}"
  RESULT_VARIABLE rc
  ERROR_VARIABLE errtxt)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "前処理に失敗 (${rc}): ${CL} ${args}\n${errtxt}")
endif()
