include $(top_srcdir)/src/mesquite/make.inc
LDADD = $(top_builddir)/itaps/mesquite/libmbmsqitaps.la \
        $(top_builddir)/src/mesquite/libmbmesquite.la \
        ${IGEOM_LDFLAGS} ${IGEOM_LIBS} \
        ${CGM_LDFLAGS} ${CGM_LIBS}

MESHDIR = $(abs_top_srcdir)/MeshFiles/mesquite

check_PROGRAMS = main
main_SOURCES = main.cpp
main_DEPENDENCIES = $(top_builddir)/itaps/mesquite/libmbmsqitaps.la \
                    $(top_builddir)/src/mesquite/libmbmesquite.la 

TESTS = main
EXTRA_DIST = CMakeLists.txt 

AM_CPPFLAGS =  -DSRCDIR=$(srcdir) \
               -DMESHDIR=$(MESHDIR) \
               $(IGEOM_DEFINES) $(IGEOM_INCLUDES) $(MSQ_INCLUDES) \
               -I$(top_srcdir)/itaps/imesh \
               -I$(top_srcdir)/src \
               -I$(top_builddir)/src \
               -I$(top_srcdir)/test \
               -I$(top_builddir)/itaps/imesh \
               -I$(top_srcdir)/itaps/mesquite \
               -I$(top_srcdir)/src/parallel

if HAVE_CGM
else
main$(EXEEXT):
	echo "ITAPS support disabled.  Reconfigure to build this test."
endif

MOSTLYCLEANFILES =	
