MESHDIR = $(abs_top_srcdir)/MeshFiles/unittest

DEFS += -DSRCDIR=$(srcdir) \
        -DMESHDIR=$(MESHDIR)

FCDEFS += $(FC_DEFINE)SRCDIR=$(srcdir) \
          $(FC_DEFINE)MESHDIR=$(MESHDIR) 

AM_CPPFLAGS  = -I$(srcdir)/.. \
               -I$(top_srcdir)/src \
               -I$(top_builddir)/src  \
               -I$(top_srcdir)/test

check_PROGRAMS = 


if PARALLEL
  AM_CPPFLAGS += -I$(top_srcdir)/src/parallel -I$(top_srcdir)/src/parallel/moab
if HAVE_HDF5_PARALLEL
# stuff for c test program
check_PROGRAMS = driver

if ENABLE_FORTRAN
  check_PROGRAMS += fex
endif
endif
endif

TESTS = $(check_PROGRAMS)

FORTDEFS = $(FCDEFS) 
PPFCCOMPILE = $(FC) $(FORTDEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
    $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_FCFLAGS) $(FCFLAGS)
LTPPFCCOMPILE = $(LIBTOOL) $(AM_V_lt) --tag=FC $(AM_LIBTOOLFLAGS) \
    $(LIBTOOLFLAGS) --mode=compile $(FC) $(FCDEFS) \
    $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) \
    $(AM_FCFLAGS) $(FCFLAGS)
PPF77COMPILE = $(F77) $(FORTDEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
   $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_FFLAGS) $(FFLAGS)
LTPPF77COMPILE = $(LIBTOOL) $(AM_V_lt) --tag=F77 $(AM_LIBTOOLFLAGS) \
   $(LIBTOOLFLAGS) --mode=compile $(F77) $(FCDEFS) \
   $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) \
   $(AM_FFLAGS) $(FFLAGS)

