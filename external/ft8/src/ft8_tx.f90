! Standalone integration wrapper. SPDX-License-Identifier: GPL-3.0-only
module ft8_tx
  use iso_c_binding
  implicit none
  private
  public :: ft_encode_wave
contains
  ! The TX caller serializes this routine because WSJT-X caches waveform state.
  integer(c_int) function ft_encode_wave(message, length, frequency, mode, sent, samples) bind(C)
    integer, parameter :: ft8_count=79*1920, ft4_count=105*576
    character(c_char), intent(in) :: message(*)
    integer(c_int), value :: length, mode
    real(c_float), value :: frequency
    character(c_char), intent(out) :: sent(37)
    integer(c_int16_t), intent(out) :: samples(*)
    character(len=37) :: input, encoded
    integer :: i, i3, n3, tones(103), count
    integer(c_int8_t) :: bits(77)
    real(c_float) :: wave(ft8_count)
    complex(c_float_complex) :: unused(1)

    ft_encode_wave=0
    if(length<1 .or. length>len(input)) return
    input=''
    do i=1,length
      input(i:i)=message(i)
    enddo
    if (mode == 4) then
      call genft4(input,0,encoded,bits,tones)
    else
      call genft8(input,i3,n3,encoded,bits,tones)
    endif
    if (encoded == '*** bad message ***' .or. len_trim(encoded)==0) return
    if (mode == 4) then
      count=ft4_count
      call gen_ft4wave(tones,103,576,12000.0,frequency,unused,wave,0,count)
    else
      count=ft8_count
      call gen_ft8wave(tones,79,1920,2.0,12000.0,frequency,unused,wave,0,count)
    endif
    samples(1:count)=nint(32767.0*wave(1:count),kind=c_int16_t)
    ft_encode_wave=len_trim(encoded)
    do i=1,ft_encode_wave
      sent(i)=encoded(i:i)
    enddo
  end function

end module
