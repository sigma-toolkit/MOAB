
#include "mpi.h"
#include  "../src/moab/imoab.h"
#include <string.h>

#define CHECKRC(rc, message)  if (0!=rc) { printf ("%s", message); return 1;}
int main(int argc, char * argv[])
{

  MPI_Init(&argc, &argv);
  int nprocs, rank;

  MPI_Comm comm=MPI_COMM_WORLD;

  MPI_Comm_size(comm, &nprocs);
  MPI_Comm_rank(comm, &rank);

  char * filen = "p8ex1.h5m";
  if (argc>1)
    filen = argv[1];

  ErrCode rc = iMOABInitialize(argc, argv);

  CHECKRC(rc, "failed to initialize MOAB");
  int num_global_vertices=0, num_global_elements=0, num_dimension=0, num_parts=0;
  rc = ReadHeaderInfo ( filen, &num_global_vertices, &num_global_elements, &num_dimension,
      &num_parts, (int)strlen(filen) );

  CHECKRC(rc, "failed to read header info");

  if (0==rank)
  {
    printf("file %s has %d vertices, %d elements, %d parts in partition\n", filen,
        num_global_vertices, num_global_elements, num_parts);
  }
  int appID;
  iMOAB_AppID pid=&appID;
  rc = RegisterApplication( "PROTEUS", &comm,  pid);
  CHECKRC(rc, "failed to register application");
  char *read_opts="PARALLEL=READ_PART;PARTITION=PARALLEL_PARTITION;PARALLEL_RESOLVE_SHARED_ENTS";
  int num_ghost_layers=1;
  rc = LoadMesh(  pid, filen, read_opts, &num_ghost_layers, strlen(filen), strlen(read_opts) );

  rc = iMOABFinalize();
  CHECKRC(rc, "failed to finalize MOAB");

  MPI_Finalize();

  return 0;
}
