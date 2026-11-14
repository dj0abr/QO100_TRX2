# Pass the payload type (8*i3, with n3 added for i3=0) through the decoder callbacks.
# 0 means free text. -1 denotes an FT8 assisted path without payload bits.
set(FT_VENDOR vendor/wsjtx/lib)
set(FT_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${FT_VENDOR}/ft8/ft8b.f90" "${FT_VENDOR}/ft8_decode.f90" "${FT_VENDOR}/ft4_decode.f90")

file(READ "${FT_VENDOR}/ft8/ft8b.f90" source)
string(REPLACE "msg37,xsnr,itone)" "msg37,xsnr,itone,message_type)" source "${source}")
string(REPLACE "  character*77 c77" "  character*77 c77\n  integer message_type" source "${source}")
string(REPLACE "     nbadcrc=0" "     message_type=8*i3+merge(n3,0,i3.eq.0)\n     nbadcrc=0" source "${source}")
file(WRITE "${FT_GENERATED}/ft8b.f90" "${source}")
list(REMOVE_ITEM WSJTX_SOURCES "${FT_VENDOR}/ft8/ft8b.f90")
list(APPEND WSJTX_SOURCES "${FT_GENERATED}/ft8b.f90")

file(READ "${FT_VENDOR}/ft8_decode.f90" source)
string(REPLACE "    use shmem, only: shmem_lock, shmem_unlock\n" "" source "${source}")
string(REPLACE "decoded,nap,qual)" "decoded,nap,qual,message_type)" source "${source}")
string(REGEX REPLACE ":: nap[ ]*\n" ":: nap, message_type\n" source "${source}")
string(REPLACE "    integer itone(NN)" "    integer message_type\n    integer itone(NN)" source "${source}")
string(REPLACE "iappass,msg37,xsnr,itone)" "iappass,msg37,xsnr,itone,message_type)" source "${source}")
string(REPLACE "call this%callback(sync,nsnr,xdt,f1,msg37,iaptype,qual)"
  "call this%callback(sync,nsnr,xdt,f1,msg37,iaptype,qual,message_type)" source "${source}")
string(REPLACE "               call this%callback(sync,nsnr,xdt,f1,msg37,iaptype,qual,message_type)"
  "               call this%callback(sync,nsnr,xdt,f1,msg37,iaptype,qual,-1)" source "${source}")
string(REPLACE "call this%callback(sync,nsnr,xdt,fbest,msg37,iaptype,qual)"
  "call this%callback(sync,nsnr,xdt,fbest,msg37,iaptype,qual,-1)" source "${source}")
file(WRITE "${FT_GENERATED}/ft8_decode.f90" "${source}")

file(READ "${FT_VENDOR}/ft4_decode.f90" source)
string(REPLACE "decoded,nap,qual)" "decoded,nap,qual,message_type)" source "${source}")
string(REGEX REPLACE ":: nap[ ]*\n" ":: nap, message_type\n" source "${source}")
string(REPLACE "      character c77*77" "      character c77*77\n      integer message_type" source "${source}")
string(REPLACE "                     call this%callback(smax,nsnr,xdt,f1,message,iaptype,qual)"
  "                     read(c77(75:77),'(b3)') i3\n                     read(c77(72:74),'(b3)') n3\n                     message_type=8*i3+merge(n3,0,i3.eq.0)\n                     call this%callback(smax,nsnr,xdt,f1,message,iaptype,qual,message_type)"
  source "${source}")
file(WRITE "${FT_GENERATED}/ft4_decode.f90" "${source}")
