module write_plotfile_module

  use layout_module
  use multifab_module
  use fabio_module

  implicit none

contains
  
  subroutine write_plotfile(la,phi,istep,dx,time,prob_lo,prob_hi)

    type(layout),   intent(in) :: la
    type(multifab), intent(in) :: phi
    integer,        intent(in) :: istep
    real(dp_t),     intent(in) :: dx, time
    real(dp_t),     intent(in) :: prob_lo(phi%dim), prob_hi(phi%dim)

    character(len=8)  :: plotfile_name
    character(len=20) :: variable_names(2)
    type(multifab)    :: plotdata(1)
    integer           :: rr(0)
    real(dp_t)        :: dx_vec(phi%dim)

    dx_vec(:)      = dx
    variable_names = [ "density", "signal " ] 

    ! build plotdata, copy state
    call build(plotdata(1),la,2,0)
    call copy(plotdata(1),phi)

    ! define the name of the plotfile that will be written
    write(unit=plotfile_name,fmt='("plt",i5.5)') istep

    ! write the plotfile
    call fabio_ml_multifab_write_d(plotdata, rr, plotfile_name, variable_names, &
                                   la%lap%pd, prob_lo, prob_hi, time, dx_vec)

    call destroy(plotdata(1))

  end subroutine write_plotfile

end module write_plotfile_module
