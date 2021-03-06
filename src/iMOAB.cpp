/** \file iMOAB.cpp
*/

#include "moab/MOABConfig.h"
#include "moab/Core.hpp"

using namespace moab;

#ifdef MOAB_HAVE_MPI
#  include "moab_mpi.h"
#  include "moab/ParallelComm.hpp"
#  include "moab/ParCommGraph.hpp"
#endif

#include <assert.h>
#include "moab/iMOAB.h"

/*
 this is needed so far because of direct access to hdf5/mhdf
  */
#ifdef MOAB_HAVE_HDF5
#include "mhdf.h"
#include <H5Tpublic.h>
#endif

#include <stdio.h>

#include <iostream>

#include "MBTagConventions.hpp"
#include "moab/MeshTopoUtil.hpp"
#include "moab/ReadUtilIface.hpp"
#include "moab/MergeMesh.hpp"
// we need to recompute adjacencies for merging to work
#include "AEntityFactory.hpp"
#include <sstream>

// global variables ; should they be organized in a structure, for easier references?
// or how do we keep them global?


#ifdef __cplusplus
extern "C" {
#endif

struct appData {
  EntityHandle file_set;
  int   external_id;  // external component id, unique for application
  Range all_verts;
  Range local_verts; // it could include shared, but not owned at the interface
                     // these vertices would be all_verts if no ghosting was required
  Range ghost_vertices; // locally ghosted from other processors
  Range primary_elems;
  Range owned_elems;
  Range ghost_elems;
  int dimension; // 2 or 3, dimension of primary elements (redundant?)
  long num_global_elements; // reunion of all elements in primary_elements; either from hdf5 reading or from reduce
  long num_global_vertices; // reunion of all nodes, after sharing is resolved; it could be determined from hdf5 reading
  Range mat_sets;
  std::map<int, int> matIndex; // map from global block id to index in mat_sets
  Range neu_sets;
  Range diri_sets;
  std::map< std::string, Tag> tagMap;
  std::vector<Tag>  tagList;

#ifdef MOAB_HAVE_MPI
  std::vector<ParCommGraph*> pgraph; // created in order of other applications that communicate with this one
  // constructor for this ParCommGraph takes the joint comm and the MPI groups for each application
#endif

 };

struct GlobalContext {
  // are there reasons to have multiple moab inits? Is ref count needed?
  Interface * MBI;
  // we should also have the default tags stored, initialized
  Tag material_tag, neumann_tag, dirichlet_tag, globalID_tag; // material, neumann, dirichlet,  globalID
  int refCountMB;
  int iArgc;
  iMOAB_String * iArgv;
  int unused_pid;

  std::map<std::string, int> appIdMap;     // from app string (uppercase) to app id
  std::map<int, int> appIdCompMap;         // from component id to app id

  #ifdef MOAB_HAVE_MPI
  std::vector<ParallelComm*> pcomms; // created in order of applications, one moab::ParallelComm for each
  #endif

  std::vector<appData> appDatas; // the same order as pcomms

  GlobalContext() {MBI=0; refCountMB =0; unused_pid=0; }
}  ;

static struct GlobalContext context;


ErrCode iMOAB_Initialize( int argc, iMOAB_String* argv )
{
   context.iArgc = argc;
   context.iArgv = argv; // shallow copy
   if (0==context.refCountMB)
   {
     context.MBI = new Core();
     // retrieve the default tags
     const char* const shared_set_tag_names[] = {MATERIAL_SET_TAG_NAME,
                                                 NEUMANN_SET_TAG_NAME,
                                                 DIRICHLET_SET_TAG_NAME,
                                                 GLOBAL_ID_TAG_NAME};
     // blocks, visible surfaceBC(neumann), vertexBC (Dirichlet), global id, parallel partition
     Tag gtags[4];
     for (int i = 0; i < 4; i++) {

       ErrorCode rval = context.MBI->tag_get_handle(shared_set_tag_names[i], 1, MB_TYPE_INTEGER,
                                           gtags[i], MB_TAG_ANY);
       if (MB_SUCCESS!=rval)
         return 1;
     }
     context.material_tag = gtags[0];
     context.neumann_tag = gtags[1];
     context.dirichlet_tag = gtags[2];
     context.globalID_tag = gtags[3];
   }
   context.refCountMB++;
   return 0;
}

ErrCode iMOAB_InitializeFortran()
{
  return iMOAB_Initialize(0, 0);
}

ErrCode iMOAB_Finalize()
{
   context.refCountMB--;
   if (0==context.refCountMB)
      delete context.MBI;
   return MB_SUCCESS;
}

ErrCode iMOAB_RegisterApplication( const iMOAB_String app_name,
#ifdef MOAB_HAVE_MPI
    MPI_Comm* comm,
#endif
    int * compid,
    iMOAB_AppID pid )
{
  // will create a parallel comm for this application too, so there will be a
  // mapping from *pid to file set and to parallel comm instances
  std::string name(app_name);
  if (context.appIdMap.find(name)!=context.appIdMap.end())
  {
    std::cout << " application " << name << " already registered \n";
    return 1;
  }
  *pid =  context.unused_pid++;
  context.appIdMap[name] = *pid;
  if (*compid <= 0)
  {
    std::cout << " convention for external application is to have its id positive \n";
    return 1;
  }
  if (context.appIdCompMap.find(*compid)!=context.appIdCompMap.end())
  {
    std::cout << " external application with comp id " << *compid << " is already registered\n";
    return 1;
  }
  context.appIdCompMap[*compid] = *pid;

  // now create ParallelComm and a file set for this application
#ifdef MOAB_HAVE_MPI
  ParallelComm * pco = new ParallelComm(context.MBI, *comm);

#ifndef NDEBUG
  int index = pco->get_id(); // it could be useful to get app id from pcomm instance ...
  assert(index==*pid);
#endif
  context.pcomms.push_back(pco);
#endif

  // create now the file set that will be used for loading the model in
  EntityHandle file_set;
  ErrorCode rval = context.MBI->create_meshset(MESHSET_SET, file_set);
  if (MB_SUCCESS != rval )
    return 1;
  appData app_data;
  app_data.file_set=file_set;
  app_data.external_id = * compid; // will be used mostly for par comm graph
  context.appDatas.push_back(app_data); // it will correspond to app_FileSets[*pid] will be the file set of interest
  return 0;
}

ErrCode iMOAB_RegisterFortranApplication( const iMOAB_String app_name,
#ifdef MOAB_HAVE_MPI
    int* comm,
#endif
    int *compid, iMOAB_AppID pid, int app_name_length )
{
  std::string name(app_name);
  if ( (int)strlen(app_name) >app_name_length )
  {
    std::cout << " length of string issue \n";
    return 1;
  }
  if (context.appIdMap.find(name)!=context.appIdMap.end())
  {
    std::cout << " application " << name << " already registered \n";
    return 1;
  }
  *pid =  context.unused_pid++;
  context.appIdMap[name] = *pid;
  if (context.appIdCompMap.find(*compid)!=context.appIdCompMap.end())
  {
    std::cout << " external application with comp id " << *compid << " is already registered\n";
    return 1;
  }
  context.appIdCompMap[*compid] = *pid;

#ifdef MOAB_HAVE_MPI
  // now create ParallelComm and a file set for this application
  // convert from fortran communicator to a c communicator
  // see transfer of handles
  // http://www.mpi-forum.org/docs/mpi-2.2/mpi22-report/node361.htm
  MPI_Comm ccomm = MPI_Comm_f2c( (MPI_Fint) *comm);
  ParallelComm * pco = new ParallelComm(context.MBI, ccomm);

#ifndef NDEBUG
  int index = pco->get_id(); // it could be useful to get app id from pcomm instance ...
  assert(index==*pid);
#endif
  context.pcomms.push_back(pco);
#endif

  // create now the file set that will be used for loading the model in
  EntityHandle file_set;
  ErrorCode rval = context.MBI->create_meshset(MESHSET_SET, file_set);
  if (MB_SUCCESS != rval )
    return 1;
  appData app_data;
  app_data.file_set=file_set;
  app_data.external_id = * compid; // will be used mostly for par comm graph
  context.appDatas.push_back(app_data); // it will correspond to app_FileSets[*pid] will be the file set of interest
  return 0;
}

ErrCode iMOAB_DeregisterApplication( iMOAB_AppID pid )
{
        // the file set , parallel comm are all in vectors indexed by *pid
  // assume we did not delete anything yet
  // *pid will not be reused if we register another application

  EntityHandle fileSet = context.appDatas[*pid].file_set;
  // get all entities part of the file set
  Range fileents;
  ErrorCode rval = context.MBI->get_entities_by_handle(fileSet, fileents, /*recursive */true);
  if (MB_SUCCESS != rval )
    return 1;

  fileents.insert(fileSet);

  rval = context.MBI->get_entities_by_type(fileSet, MBENTITYSET, fileents); // append all mesh sets
  if (MB_SUCCESS != rval )
    return 1;
#ifdef MOAB_HAVE_MPI
  ParallelComm * pco = context.pcomms[*pid];
  // we could get the pco also with
  // ParallelComm * pcomm = ParallelComm::get_pcomm(context.MBI, *pid);
  delete pco;
  std::vector<ParCommGraph*> & pargs = context.appDatas[*pid].pgraph;
  // free the parallel comm graphs associated with this app
  for (size_t k=0; k<pargs.size(); k++)
    delete pargs[k];
#endif

  rval = context.MBI->delete_entities(fileents);

  if (MB_SUCCESS != rval )
    return 1;

  return 0;
}

ErrCode iMOAB_ReadHeaderInfo ( const iMOAB_String filename, int* num_global_vertices, int* num_global_elements, int* num_dimension, int* num_parts, int filename_length )
{
#ifdef MOAB_HAVE_HDF5
  std::string filen(filename);
  if (filename_length < (int)strlen(filename))
  {
    filen = filen.substr(0,filename_length);
  }
  *num_global_vertices = 0;
  int edges = 0;
  int faces = 0;
  int regions = 0;
  *num_global_elements =0;
  *num_dimension = 0;
  *num_parts = 0;

  mhdf_FileHandle file;
  mhdf_Status status;
  unsigned long max_id;
  struct mhdf_FileDesc* data;

  file = mhdf_openFile( filen.c_str() , 0, &max_id, -1, &status );
  if (mhdf_isError( &status )) {
    fprintf( stderr,"%s: %s\n", filename, mhdf_message( &status ) );
    return 1;
  }

  data = mhdf_getFileSummary( file, H5T_NATIVE_ULONG, &status, 1); // will use extra set info; will get parallel partition tag info too!
  if (mhdf_isError( &status )) {
    fprintf( stderr,"%s: %s\n", filename, mhdf_message( &status ) );
    return 1;
  }
  *num_dimension = data->nodes.vals_per_ent;
  *num_global_vertices = (int)data->nodes.count;

  for (int i=0; i<data->num_elem_desc; i++)
  {
    struct mhdf_ElemDesc * el_desc = &(data->elems[i]);
    struct mhdf_EntDesc * ent_d = &(el_desc->desc);
    if (0==strcmp(el_desc->type, mhdf_EDGE_TYPE_NAME)) edges += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_TRI_TYPE_NAME))  faces += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_QUAD_TYPE_NAME)) faces += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_POLYGON_TYPE_NAME)) faces += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_TET_TYPE_NAME)) regions += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_PYRAMID_TYPE_NAME)) regions += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_PRISM_TYPE_NAME)) regions += ent_d->count;
    if (0==strcmp(el_desc->type, mdhf_KNIFE_TYPE_NAME)) regions += ent_d->count;
    if (0==strcmp(el_desc->type, mdhf_HEX_TYPE_NAME)) regions += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_POLYHEDRON_TYPE_NAME)) regions += ent_d->count;
    if (0==strcmp(el_desc->type, mhdf_SEPTAHEDRON_TYPE_NAME)) regions += ent_d->count;
  }
  *num_parts = data->numEntSets[0];

  // is this required?
  if (edges >0 ){
    *num_dimension = 1; // I don't think it will ever return 1
    *num_global_elements = edges;
  }
  if (faces >0 ){
    *num_dimension = 2;
    *num_global_elements = faces;
  }
  if (regions>0){
    *num_dimension = 3;
    *num_global_elements = regions;
  }
  mhdf_closeFile( file, &status );

  free( data );

#else
  std::cout << " cannot retrieve header info except for h5m file \n";
#endif

  return 0;
}

ErrCode iMOAB_LoadMesh( iMOAB_AppID pid, const iMOAB_String filename, const iMOAB_String read_options, int * num_ghost_layers, int filename_length, int read_options_length )
{
  if ( (int)strlen(filename) > filename_length)
  {
    std::cout<<" filename length issue\n";
    return 1;
  }
  if ( (int)strlen(read_options) > read_options_length)
  {
    std::cout<<" read options length issue\n";
    return 1;
  }
  // make sure we use the file set and pcomm associated with the *pid
  std::ostringstream newopts;
  newopts  << read_options;
#ifdef MOAB_HAVE_MPI
  newopts << ";PARALLEL_COMM="<<*pid;
  if (*num_ghost_layers>=1)
  {
    // if we want ghosts, we will want additional entities, the last .1
    // because the addl ents can be edges, faces that are part of the neumann sets
    newopts << ";PARALLEL_GHOSTS=3.0."<<*num_ghost_layers<<".3";
  }
#else
  *num_ghost_layers = 0; // do not use in case of serial run
#endif
  ErrorCode rval = context.MBI->load_file(filename, &context.appDatas[*pid].file_set, newopts.str().c_str());
  if (MB_SUCCESS!=rval)
    return 1;

#ifndef NDEBUG

#ifdef MOAB_HAVE_MPI
  int rank = context.pcomms[*pid]->rank();
  int nprocs = context.pcomms[*pid]->size();
#endif

  // some debugging stuff
  std::ostringstream outfile;
#ifdef MOAB_HAVE_MPI
  outfile <<"TaskMesh_n" <<nprocs<<"."<< rank<<".h5m";
#else
  outfile <<"TaskMesh_n1.0.h5m";
#endif
  // the mesh contains ghosts too, but they are not part of mat/neumann set
  // write in serial the file, to see what tags are missing
  rval = context.MBI->write_file(outfile.str().c_str()); // everything on root
  if (MB_SUCCESS!=rval)
    return 1;
#endif
  int rc = iMOAB_UpdateMeshInfo(pid);
  return rc;
}

ErrCode iMOAB_WriteMesh( iMOAB_AppID pid, iMOAB_String filename, iMOAB_String write_options, int filename_length, int write_options_length )
{
  // maybe do some processing of strings and lengths
  if ( (int) strlen(filename) > filename_length)
  {
    std::cout << " file name length issue\n";
    return 1;
  }
  if ( (int) strlen(write_options) > write_options_length)
  {
    std::cout << " write options issue\n";
    return 1;
  }
  // maybe do some options processing?
  ErrorCode rval = context.MBI->write_file(filename,0, write_options,  &context.appDatas[*pid].file_set, 1);
  if (MB_SUCCESS!=rval)
    return 1;
  return 0;
}


ErrCode iMOAB_UpdateMeshInfo(iMOAB_AppID pid)
{
  // this will include ghost elements info
    //
    appData & data = context.appDatas[*pid];
    EntityHandle fileSet=data.file_set;
    // first clear all data ranges; this can be called after ghosting
    data.all_verts.clear();
    data.primary_elems.clear();
    data.local_verts.clear();
    data.ghost_vertices.clear();
    data.owned_elems.clear();
    data.ghost_elems.clear();
    data.mat_sets.clear();
    data.neu_sets.clear();
    data.diri_sets.clear();

    ErrorCode rval = context.MBI->get_entities_by_type(fileSet, MBVERTEX, data.all_verts, true); // recursive
    if (MB_SUCCESS!=rval)
      return 1;

    rval = context.MBI->get_entities_by_dimension(fileSet, 3, data.primary_elems, true); // recursive
    if (MB_SUCCESS!=rval)
      return 1;
    data.dimension = 3;
    if (data.primary_elems.empty())
    {
      context.appDatas[*pid].dimension = 2;
      rval = context.MBI->get_entities_by_dimension(fileSet, 2, data.primary_elems, true); // recursive
      if (MB_SUCCESS!=rval)
        return 1;
      if (data.primary_elems.empty())
      {
        context.appDatas[*pid].dimension = 1;
        rval = context.MBI->get_entities_by_dimension(fileSet, 1, data.primary_elems, true); // recursive
        if (MB_SUCCESS!=rval)
          return 1;
        if (data.primary_elems.empty())
          return 1; // no elements of dimension 1 or 2 or 3
      }
    }

  #ifdef MOAB_HAVE_MPI
    ParallelComm * pco = context.pcomms[*pid];

    // filter ghost vertices, from local
    rval = pco -> filter_pstatus(data.all_verts, PSTATUS_GHOST, PSTATUS_NOT, -1, &data.local_verts);
    if (MB_SUCCESS!=rval)
      return 1;
    data.ghost_vertices = subtract(data.all_verts, data.local_verts);

    // get all blocks, BCs, etc

    // filter ghost elements, from local
    rval = pco -> filter_pstatus(data.primary_elems, PSTATUS_GHOST, PSTATUS_NOT, -1, &data.owned_elems);
    if (MB_SUCCESS!=rval)
      return 1;
    data.ghost_elems = subtract(data.primary_elems, data.owned_elems);

  #else

    data.local_verts = data.all_verts;
    data.owned_elems = data.primary_elems;

  #endif
  rval = context.MBI->get_entities_by_type_and_tag(fileSet, MBENTITYSET, &(context.material_tag), 0, 1, data.mat_sets , Interface::UNION);
  if (MB_SUCCESS!=rval)
    return 1;

  rval = context.MBI->get_entities_by_type_and_tag(fileSet, MBENTITYSET, &(context.neumann_tag), 0, 1, data.neu_sets , Interface::UNION);
  if (MB_SUCCESS!=rval)
    return 1;

  rval = context.MBI->get_entities_by_type_and_tag(fileSet, MBENTITYSET, &(context.dirichlet_tag), 0, 1, data.diri_sets , Interface::UNION);
  if (MB_SUCCESS!=rval)
    return 1;


  return 0;
}
ErrCode iMOAB_GetMeshInfo( iMOAB_AppID pid, int* num_visible_vertices, int* num_visible_elements, int *num_visible_blocks, int* num_visible_surfaceBC, int* num_visible_vertexBC )
{

  // this will include ghost elements
  //
  appData & data = context.appDatas[*pid];
  EntityHandle fileSet=data.file_set;
  // first clear all data ranges; this can be called after ghosting

  num_visible_elements[2] = (int) data.primary_elems.size();
  // separate ghost and local/owned primary elements
  num_visible_elements[0] = (int)data.owned_elems.size();
  num_visible_elements[1] = (int)data.ghost_elems.size();
  num_visible_vertices[2] = (int)data.all_verts.size();
  num_visible_vertices[1] = (int)data.ghost_vertices.size();
  num_visible_vertices[0] =  num_visible_vertices[2] - num_visible_vertices[1]; // local are those that are not ghosts

  ErrorCode rval = context.MBI->get_entities_by_type_and_tag(fileSet, MBENTITYSET, &(context.material_tag), 0, 1, data.mat_sets , Interface::UNION);
  if (MB_SUCCESS!=rval)
    return 1;
  num_visible_blocks[2] = data.mat_sets.size();
  num_visible_blocks[0] = num_visible_blocks[2];
  num_visible_blocks[1] = 0;
  rval = context.MBI->get_entities_by_type_and_tag(fileSet, MBENTITYSET, &(context.neumann_tag), 0, 1, data.neu_sets , Interface::UNION);
  if (MB_SUCCESS!=rval)
    return 1;
  num_visible_surfaceBC[2] = 0;
  // count how many faces are in each neu set, and how many regions are
  // adjacent to them;
  int numNeuSets = (int)data.neu_sets.size();
  for (int i=0; i<numNeuSets; i++)
  {
    Range subents;
    EntityHandle nset = data.neu_sets[i];
    rval = context.MBI->get_entities_by_dimension(nset, data.dimension-1, subents);
    if (MB_SUCCESS!=rval)
      return 1;
    for (Range::iterator it=subents.begin(); it!=subents.end(); ++it)
    {
      EntityHandle subent = *it;
      Range adjPrimaryEnts;
      rval = context.MBI->get_adjacencies(&subent, 1, data.dimension, false, adjPrimaryEnts);
      if (MB_SUCCESS!=rval)
        return 1;
      num_visible_surfaceBC[2] += (int)adjPrimaryEnts.size();
    }
  }
  num_visible_surfaceBC[0] = num_visible_surfaceBC[2];
  num_visible_surfaceBC[1] = 0; //
  rval = context.MBI->get_entities_by_type_and_tag(fileSet, MBENTITYSET, &(context.dirichlet_tag), 0, 1, data.diri_sets , Interface::UNION);
  if (MB_SUCCESS!=rval)
    return 1;
  num_visible_vertexBC[2]= 0;
  int numDiriSets = (int)data.diri_sets.size();
  for (int i=0; i<numDiriSets; i++)
  {
    Range verts;
    EntityHandle diset = data.diri_sets[i];
    rval = context.MBI->get_entities_by_dimension(diset, 0, verts);
    if (MB_SUCCESS!=rval)
      return 1;
    num_visible_vertexBC[2] += (int)verts.size();
  }
  num_visible_vertexBC[0] = num_visible_vertexBC[2];
  num_visible_vertexBC[1] = 0;

  return 0;
}

ErrCode iMOAB_GetVertexID( iMOAB_AppID pid, int * vertices_length, iMOAB_GlobalID* global_vertex_ID)
{
//
  Range & verts = context.appDatas[*pid].all_verts;
  if ((int)verts.size()!=*vertices_length)
      return 1; // problem with array length
  // global id tag is context.globalID_tag
  ErrorCode rval = context.MBI->tag_get_data(context.globalID_tag, verts, global_vertex_ID);
  if (MB_SUCCESS!=rval)
    return 1;

  return 0;
}

ErrCode iMOAB_GetVertexOwnership( iMOAB_AppID pid, int *vertices_length, int* visible_global_rank_ID )
{
  Range & verts = context.appDatas[*pid].all_verts;
  int i=0;
#ifdef MOAB_HAVE_MPI
  ParallelComm * pco = context.pcomms[*pid];
  for (Range::iterator vit=verts.begin(); vit!=verts.end(); vit++, i++)
  {
    ErrorCode rval = pco->  get_owner(*vit, visible_global_rank_ID[i]);
    if (MB_SUCCESS!=rval)
      return 1;
  }
  if (i!=*vertices_length)
    return 1; // warning array allocation problem
#else
  /* everything owned by proc 0 */
  if ((int)verts.size()!=*vertices_length)
      return 1; // warning array allocation problem
  for (Range::iterator vit=verts.begin(); vit!=verts.end(); vit++, i++)
    visible_global_rank_ID[i] = 0; // all vertices are owned by processor 0, as this is serial run
#endif
  return 0;
}

ErrCode iMOAB_GetVisibleVerticesCoordinates( iMOAB_AppID pid, int * coords_length, double* coordinates )
{
  Range & verts = context.appDatas[*pid].all_verts;
  // interleaved coordinates, so that means deep copy anyway
  if (*coords_length!=3*(int)verts.size())
    return 1;
  ErrorCode rval = context.MBI->get_coords(verts, coordinates);
  if (MB_SUCCESS!=rval)
    return 1;
  return 0;
}

ErrCode iMOAB_GetBlockID( iMOAB_AppID pid, int * block_length, iMOAB_GlobalID* global_block_IDs)
{
  // local id blocks? they are counted from 0 to number of visible blocks ...
  // will actually return material set tag value for global
  Range & matSets = context.appDatas[*pid].mat_sets;
  if (*block_length!=(int)matSets.size())
    return 1;
  // return material set tag gtags[0 is material set tag
  ErrorCode rval = context.MBI->tag_get_data(context.material_tag, matSets, global_block_IDs);
  if (MB_SUCCESS!=rval)
    return 1;
  // populate map with index
  std::map <int, int> & matIdx = context.appDatas[*pid].matIndex;
  //
  for (int i=0; i<(int)matSets.size(); i++)
  {
    matIdx[global_block_IDs[i]] = i;
  }
  return 0;
}

ErrCode iMOAB_GetBlockInfo(iMOAB_AppID pid, iMOAB_GlobalID * global_block_ID,
    int* vertices_per_element, int* num_elements_in_block)
{
  std::map<int, int> & matMap = context.appDatas[*pid].matIndex;
  std::map<int,int>::iterator it = matMap.find(*global_block_ID);
  if (it==matMap.end())
    return 1; // error in finding block with id
  int blockIndex = matMap[*global_block_ID];
  EntityHandle matMeshSet = context.appDatas[*pid].mat_sets[blockIndex];
  Range blo_elems;
  ErrorCode rval = context.MBI-> get_entities_by_handle(matMeshSet, blo_elems);
  if (MB_SUCCESS!=rval ||  blo_elems.empty() )
    return 1;

  EntityType type = context.MBI->type_from_handle(blo_elems[0]);
  if (!blo_elems.all_of_type(type))
    return 1; //not all of same  type

  const EntityHandle * conn=NULL;
  int num_verts=0;
  rval = context.MBI->get_connectivity(blo_elems[0], conn, num_verts);
  if (MB_SUCCESS!=rval)
    return 1;
  *vertices_per_element=num_verts;
  *num_elements_in_block = (int)blo_elems.size();

  return 0;
}

ErrCode iMOAB_GetVisibleElementsInfo(iMOAB_AppID pid, int* num_visible_elements,
    iMOAB_GlobalID * element_global_IDs, int * ranks, iMOAB_GlobalID * block_IDs)
{
  appData & data =  context.appDatas[*pid];
#ifdef MOAB_HAVE_MPI
  ParallelComm * pco = context.pcomms[*pid];
#endif

  ErrorCode rval = context.MBI-> tag_get_data(context.globalID_tag, data.primary_elems, element_global_IDs);
  if (MB_SUCCESS!=rval)
    return 1;

  int i=0;
  for (Range::iterator eit=data.primary_elems.begin(); eit!=data.primary_elems.end(); ++eit, ++i)
  {
#ifdef MOAB_HAVE_MPI
    rval = pco->get_owner(*eit, ranks[i]);
    if (MB_SUCCESS!=rval)
      return 1;
#else
    /* everything owned by task 0 */
    ranks[i] = 0;
#endif
  }
  for (Range::iterator mit=data.mat_sets.begin(); mit!=data.mat_sets.end(); ++mit)
  {
    EntityHandle matMeshSet = *mit;
    Range elems;
    rval = context.MBI-> get_entities_by_handle(matMeshSet, elems);
    if (MB_SUCCESS!=rval )
      return 1;
    int valMatTag;
    rval = context.MBI->tag_get_data(context.material_tag, &matMeshSet, 1, &valMatTag);
    if (MB_SUCCESS!=rval )
      return 1;

    for (Range::iterator eit=elems.begin(); eit!=elems.end(); ++eit)
    {
      EntityHandle eh=*eit;
      int index=data.primary_elems.index(eh);
      if (-1==index)
        return 1;
      if (-1>= *num_visible_elements)
        return 1;
      block_IDs[index]=valMatTag;
    }
  }


  return 0;
}

ErrCode iMOAB_GetBlockElementConnectivities(iMOAB_AppID pid, iMOAB_GlobalID * global_block_ID, int * connectivity_length, int* element_connectivity)
{
  appData & data =  context.appDatas[*pid];
  std::map<int, int> & matMap = data.matIndex;
  std::map<int,int>::iterator it = matMap.find(*global_block_ID);
  if (it==matMap.end())
    return 1; // error in finding block with id
  int blockIndex = matMap[*global_block_ID];
  EntityHandle matMeshSet = data.mat_sets[blockIndex];
  std::vector<EntityHandle> elems;

  ErrorCode rval = context.MBI-> get_entities_by_handle(matMeshSet, elems);
  if (MB_SUCCESS!=rval ||  elems.empty() )
    return 1;


  std::vector<EntityHandle> vconnect;
  rval = context.MBI->get_connectivity(&elems[0], elems.size(), vconnect);
  if (MB_SUCCESS!=rval)
    return 1;
  if (*connectivity_length!=(int)vconnect.size())
    return 1; // mismatched sizes


  for (int i=0; i<*connectivity_length; i++)
  {
    int inx = data.all_verts.index(vconnect[i]);
    if (-1==inx)
      return 1; // error, vertex not in local range
    element_connectivity[i] = inx;
  }
  return 0;
}

ErrCode iMOAB_GetElementConnectivity(iMOAB_AppID pid, iMOAB_LocalID * elem_index, int * connectivity_length, int* element_connectivity)
{
  appData & data =  context.appDatas[*pid];
  assert((*elem_index >=0)  && (*elem_index< (int)data.primary_elems.size()) );
  EntityHandle eh = data.primary_elems[*elem_index];
  int num_nodes;
  const EntityHandle * conn;
  ErrorCode rval = context.MBI->get_connectivity(eh, conn, num_nodes);
  if (MB_SUCCESS!=rval)
    return 1;
  if (* connectivity_length < num_nodes)
    return 1; // wrong number of vertices

  for (int i=0; i<num_nodes; i++)
  {
    int index = data.all_verts.index(conn[i]);
    if (-1==index)
      return 1;
    element_connectivity[i] = index;
  }
  * connectivity_length = num_nodes;
  return 0;
}

ErrCode iMOAB_GetElementOwnership(iMOAB_AppID pid, iMOAB_GlobalID * global_block_ID, int * num_elements_in_block, int* element_ownership)
{
  std::map<int, int> & matMap = context.appDatas[*pid].matIndex;

  std::map<int,int>::iterator it = matMap.find(*global_block_ID);
  if (it==matMap.end())
    return 1; // error in finding block with id
  int blockIndex = matMap[*global_block_ID];
  EntityHandle matMeshSet = context.appDatas[*pid].mat_sets[blockIndex];
  Range elems;

  ErrorCode rval = context.MBI-> get_entities_by_handle(matMeshSet, elems);
  if (MB_SUCCESS!=rval ||  elems.empty() )
    return 1;

  if (*num_elements_in_block!=(int)elems.size())
    return 1; // bad memory allocation
  int i=0;
#ifdef MOAB_HAVE_MPI
  ParallelComm * pco = context.pcomms[*pid];
#endif
  for (Range::iterator vit=elems.begin(); vit!=elems.end(); vit++, i++)
  {
#ifdef MOAB_HAVE_MPI
    rval = pco->  get_owner(*vit, element_ownership[i]);
    if (MB_SUCCESS!=rval)
      return 1;
#else
    element_ownership[i] = 0; /* owned by 0 */
#endif
  }
  return 0;
}

ErrCode iMOAB_GetElementID(iMOAB_AppID pid, iMOAB_GlobalID * global_block_ID, int * num_elements_in_block, iMOAB_GlobalID* global_element_ID, iMOAB_LocalID* local_element_ID)
{
  appData & data = context.appDatas[*pid];
  std::map<int, int> & matMap = data.matIndex;

  std::map<int,int>::iterator it = matMap.find(*global_block_ID);
  if (it==matMap.end())
    return 1; // error in finding block with id
  int blockIndex = matMap[*global_block_ID];
  EntityHandle matMeshSet = data.mat_sets[blockIndex];
  Range elems;
  ErrorCode rval = context.MBI-> get_entities_by_handle(matMeshSet, elems);
  if (MB_SUCCESS!=rval ||  elems.empty() )
    return 1;



  if (*num_elements_in_block!=(int)elems.size())
    return 1; // bad memory allocation

  rval = context.MBI->tag_get_data(context.globalID_tag, elems, global_element_ID);
  if (MB_SUCCESS!=rval )
    return 1;

  // check that elems are among primary_elems in data
  for (int i=0; i<*num_elements_in_block; i++)
  {
    local_element_ID[i]=data.primary_elems.index(elems[i]);
    if (-1==local_element_ID[i])
      return 1;// error, not in local primary elements
  }

  return 0;
}

ErrCode iMOAB_GetPointerToSurfaceBC(iMOAB_AppID pid, int * surface_BC_length, iMOAB_LocalID* local_element_ID, int* reference_surface_ID, int* boundary_condition_value)
{
  // we have to fill bc data for neumann sets;/

  // it was counted above, in GetMeshInfo
  appData & data = context.appDatas[*pid];
  int numNeuSets = (int)data.neu_sets.size();

  int index = 0; // index [0, surface_BC_length) for the arrays returned
  for (int i=0; i<numNeuSets; i++)
  {
    Range subents;
    EntityHandle nset = data.neu_sets[i];
    ErrorCode rval = context.MBI->get_entities_by_dimension(nset, data.dimension-1, subents);
    if (MB_SUCCESS!=rval)
      return 1;
    int neuVal ;
    rval = context.MBI->tag_get_data(context.neumann_tag, &nset, 1, &neuVal);
    if (MB_SUCCESS!=rval)
      return 1;
    for (Range::iterator it=subents.begin(); it!=subents.end(); ++it)
    {
      EntityHandle subent = *it;
      Range adjPrimaryEnts;
      rval = context.MBI->get_adjacencies(&subent, 1, data.dimension, false, adjPrimaryEnts);
      if (MB_SUCCESS!=rval)
        return 1;
      // get global id of the primary ents, and side number of the quad/subentity
      // this is moab ordering
      for (Range::iterator pit=adjPrimaryEnts.begin(); pit!=adjPrimaryEnts.end(); pit++)
      {
        EntityHandle primaryEnt = *pit;
        // get global id
        /*int globalID;
        rval = context.MBI->tag_get_data(gtags[3], &primaryEnt, 1, &globalID);
        if (MB_SUCCESS!=rval)
          return 1;
        global_element_ID[index] = globalID;*/
        // get local element id
        local_element_ID[index] = data.primary_elems.index(primaryEnt);
        if (-1 == local_element_ID[index] )
          return 1; // did not find the element locally

        int side_number, sense, offset;
        rval = context.MBI->side_number(primaryEnt, subent,  side_number, sense, offset);
        if (MB_SUCCESS!=rval)
           return 1;
        reference_surface_ID[index] = side_number+1; // moab is from 0 to 5, it needs 1 to 6
        boundary_condition_value[index] = neuVal;
        index++;
      }
    }
  }
  if (index != *surface_BC_length)
    return 1; // error in array allocations

  return 0;
}

ErrCode iMOAB_GetPointerToVertexBC(iMOAB_AppID pid, int * vertex_BC_length,
    iMOAB_LocalID* local_vertex_ID, int* boundary_condition_value)
{
  // it was counted above, in GetMeshInfo
  appData & data = context.appDatas[*pid];
  int numDiriSets = (int)data.diri_sets.size();
  int index = 0; // index [0, *vertex_BC_length) for the arrays returned
  for (int i=0; i<numDiriSets; i++)
  {
    Range verts;
    EntityHandle diset = data.diri_sets[i];
    ErrorCode rval = context.MBI->get_entities_by_dimension(diset, 0, verts);
    if (MB_SUCCESS!=rval)
      return 1;
    int diriVal;
    rval = context.MBI->tag_get_data(context.dirichlet_tag, &diset, 1, &diriVal);
    if (MB_SUCCESS!=rval)
      return 1;

    for (Range::iterator vit=verts.begin(); vit!=verts.end(); ++vit)
    {
      EntityHandle vt =*vit;
      /*int vgid;
      rval = context.MBI->tag_get_data(gtags[3], &vt, 1, &vgid);
      if (MB_SUCCESS!=rval)
        return 1;
      global_vertext_ID[index] = vgid;*/
      local_vertex_ID[index] = data.all_verts.index(vt);
      if (-1==local_vertex_ID[index])
        return 1; // vertex was not found
      boundary_condition_value[index] = diriVal;
      index++;
    }
  }
  if (*vertex_BC_length!=index)
    return 1; // array allocation issue

  return 0;
}

ErrCode iMOAB_DefineTagStorage(iMOAB_AppID pid, const iMOAB_String tag_storage_name, int* tag_type, int* components_per_entity, int *tag_index,  int tag_storage_name_length)
{
  // see if the tag is already existing, and if yes, check the type, length
  if (*tag_type <0 || *tag_type>5)
    return 1; // we have 6 types of tags supported so far

  DataType tagDataType;
  TagType tagType;
  void * defaultVal = NULL;
  int * defInt = new int [*components_per_entity];
  double * defDouble = new double [*components_per_entity];
  EntityHandle * defHandle = new EntityHandle[*components_per_entity];
  for (int i=0; i<*components_per_entity; i++)
  {
    defInt[i] = 0;
    defDouble[i] = 0.;
    defHandle[i] = (EntityHandle)0;
  }
  switch (*tag_type) {
    case 0: tagDataType = MB_TYPE_INTEGER; tagType = MB_TAG_DENSE; defaultVal=defInt; break;
    case 1: tagDataType = MB_TYPE_DOUBLE;  tagType = MB_TAG_DENSE; defaultVal=defDouble; break;
    case 2: tagDataType = MB_TYPE_HANDLE;  tagType = MB_TAG_DENSE; defaultVal=defHandle; break;
    case 3: tagDataType = MB_TYPE_INTEGER; tagType = MB_TAG_SPARSE; defaultVal=defInt; break;
    case 4: tagDataType = MB_TYPE_DOUBLE;  tagType = MB_TAG_SPARSE; defaultVal=defDouble; break;
    case 5: tagDataType = MB_TYPE_HANDLE;  tagType = MB_TAG_SPARSE; defaultVal=defHandle; break;
    default : {
      delete [] defInt;
      delete [] defDouble;
      delete [] defHandle;
      return 1;
    } // error
  }
  std::string tag_name(tag_storage_name);
  if (tag_storage_name_length< (int)strlen(tag_storage_name))
  {
    tag_name = tag_name.substr(0, tag_storage_name_length);
  }

  Tag tagHandle;
  ErrorCode rval = context.MBI->tag_get_handle(tag_name.c_str(), *components_per_entity,
      tagDataType,
      tagHandle, tagType, defaultVal);

  // we don't need default values anymore, avoid leaks
  delete [] defInt;
  delete [] defDouble;
  delete [] defHandle;

  appData & data = context.appDatas[*pid];
  if (MB_ALREADY_ALLOCATED==rval)
  {
    std::map<std::string, Tag> & mTags = data.tagMap;
    std::map<std::string, Tag>::iterator mit = mTags.find(tag_name);
    if (mit==mTags.end())
    {
      // add it to the map
      mTags[tag_name] = tagHandle;
      // push it to the list of tags, too
      *tag_index = (int)data.tagList.size();
      data.tagList.push_back(tagHandle) ;
    }
    return 0; // OK, we found it, and we have it stored in the map tag
  }
  else if (MB_SUCCESS == rval)
  {
    data.tagMap[tag_name] = tagHandle;
    *tag_index = (int)data.tagList.size();
    data.tagList.push_back(tagHandle) ;
    return 0;
  }
  return 1; // some error, maybe the tag was not created
}

ErrCode iMOAB_SetIntTagStorage(iMOAB_AppID pid, const iMOAB_String tag_storage_name,
    int * num_tag_storage_length, int * ent_type, int* tag_storage_data,
    int tag_storage_name_length)
{
  std::string tag_name(tag_storage_name);
  if (tag_storage_name_length< (int)strlen(tag_storage_name))
  {
    tag_name = tag_name.substr(0, tag_storage_name_length);
  }
  appData & data = context.appDatas[*pid];
  if (data.tagMap.find(tag_name)== data.tagMap.end())
    return 1; // tag not defined
  Tag tag =  data.tagMap[tag_name];

  int tagLength =0;
  ErrorCode rval = context.MBI->tag_get_length(tag, tagLength);
  if (MB_SUCCESS!=rval)
    return 1;
  DataType  dtype;
  rval = context.MBI->tag_get_data_type(tag, dtype);
  if (MB_SUCCESS!=rval || dtype!=MB_TYPE_INTEGER)
    return 1;
  // set it on a subset of entities, based on type and length
  Range * ents_to_set;
  if (*ent_type == 0)// vertices
    ents_to_set = &data.all_verts;
  else  // if (*ent_type == 1) // *ent_type can be 0 (vertices) or 1 (elements)
    ents_to_set = &data.primary_elems;

  int nents_to_be_set = *num_tag_storage_length /tagLength;

  if (nents_to_be_set > (int)ents_to_set->size() || nents_to_be_set<1)
    return 1; // to many entities to be set or too few
  // restrict the range; everything is contiguous; or not?

  Range contig_range( *(ents_to_set->begin()), *(ents_to_set->begin()+nents_to_be_set-1));
  rval = context.MBI->tag_set_data(tag, contig_range, tag_storage_data);
  if (MB_SUCCESS!=rval)
    return 1;

  return 0; // no error
}

ErrCode iMOAB_GetIntTagStorage(iMOAB_AppID pid, const iMOAB_String tag_storage_name, int *num_tag_storage_length, int * ent_type, int* tag_storage_data, int tag_storage_name_length)
{
  std::string tag_name(tag_storage_name);
  if (tag_storage_name_length< (int)tag_name.length())
  {
    tag_name = tag_name.substr(0, tag_storage_name_length);
  }
  appData & data = context.appDatas[*pid];
  if (data.tagMap.find(tag_name)== data.tagMap.end())
    return 1; // tag not defined
  Tag tag =  data.tagMap[tag_name];

  int tagLength =0;
  ErrorCode rval = context.MBI->tag_get_length(tag, tagLength);
  if (MB_SUCCESS!=rval)
    return 1;
  DataType  dtype;
  rval = context.MBI->tag_get_data_type(tag, dtype);
  if (MB_SUCCESS!=rval || dtype!=MB_TYPE_INTEGER)
    return 1;

  // set it on a subset of entities, based on type and length
  Range * ents_to_get;
  if (*ent_type == 0)// vertices
    ents_to_get = &data.all_verts;
  else  // if (*ent_type == 1)
    ents_to_get = &data.primary_elems;

  int nents_to_get = *num_tag_storage_length /tagLength;

  if (nents_to_get > (int)ents_to_get->size() || nents_to_get<1)
    return 1; // to many entities to get, or too little
  // restrict the range; everything is contiguous; or not?

  Range contig_range( *(ents_to_get->begin()), *(ents_to_get->begin()+nents_to_get-1));

  rval = context.MBI->tag_get_data(tag, contig_range, tag_storage_data);
  if (MB_SUCCESS!=rval)
    return 1;

  return 0; // no error
}

ErrCode iMOAB_SetDoubleTagStorage(iMOAB_AppID pid, const iMOAB_String tag_storage_name, int * num_tag_storage_length, int * ent_type, double* tag_storage_data, int tag_storage_name_length)
{
  // exactly the same code as for int tag :) maybe should check the type of tag too
  std::string tag_name(tag_storage_name);
  if (tag_storage_name_length< (int)tag_name.length())
  {
    tag_name = tag_name.substr(0, tag_storage_name_length);
  }
  appData & data = context.appDatas[*pid];
  if (data.tagMap.find(tag_name)== data.tagMap.end())
    return 1; // tag not defined
  Tag tag =  data.tagMap[tag_name];

  int tagLength =0;
  ErrorCode rval = context.MBI->tag_get_length(tag, tagLength);
  if (MB_SUCCESS!=rval)
    return 1;

  DataType  dtype;
  rval = context.MBI->tag_get_data_type(tag, dtype);
  if (MB_SUCCESS!=rval || dtype!=MB_TYPE_DOUBLE)
    return 1;

  // set it on a subset of entities, based on type and length
  Range * ents_to_set = NULL;
  if (* ent_type == 0)// vertices
    ents_to_set = &data.all_verts;
  else if (* ent_type == 1)
    ents_to_set = &data.primary_elems;

  int nents_to_be_set = *num_tag_storage_length /tagLength;

  if (nents_to_be_set > (int)ents_to_set->size() || nents_to_be_set<1)
    return 1; // to many entities to be set
  // restrict the range; everything is contiguous; or not?

  Range contig_range( *(ents_to_set->begin()), *(ents_to_set->begin()+nents_to_be_set-1));

  rval = context.MBI->tag_set_data(tag, contig_range, tag_storage_data);
  if (MB_SUCCESS!=rval)
    return 1;

  return 0; // no error
}

ErrCode iMOAB_GetDoubleTagStorage(iMOAB_AppID pid, const iMOAB_String tag_storage_name, int * num_tag_storage_length, int * ent_type, double* tag_storage_data, int tag_storage_name_length)
{
  // exactly the same code, except tag type check
  std::string tag_name(tag_storage_name);
  if (tag_storage_name_length< (int)tag_name.length())
  {
    tag_name = tag_name.substr(0, tag_storage_name_length);
  }
  appData & data = context.appDatas[*pid];
  if (data.tagMap.find(tag_name)== data.tagMap.end())
    return 1; // tag not defined
  Tag tag =  data.tagMap[tag_name];

  int tagLength =0;
  ErrorCode rval = context.MBI->tag_get_length(tag, tagLength);
  if (MB_SUCCESS!=rval)
    return 1;

  DataType  dtype;
  rval = context.MBI->tag_get_data_type(tag, dtype);
  if (MB_SUCCESS!=rval || dtype!=MB_TYPE_DOUBLE)
    return 1;

  // set it on a subset of entities, based on type and length
  Range * ents_to_get = NULL;
  if (* ent_type == 0)// vertices
    ents_to_get = &data.all_verts;
  else if (* ent_type == 1)
    ents_to_get = &data.primary_elems;

  int nents_to_get = *num_tag_storage_length /tagLength;

  if (nents_to_get > (int)ents_to_get->size() || nents_to_get<1)
    return 1; // to many entities to get
  // restrict the range; everything is contiguous; or not?

  Range contig_range( *(ents_to_get->begin()), *(ents_to_get->begin()+nents_to_get-1));
  rval = context.MBI->tag_get_data(tag, contig_range, tag_storage_data);
  if (MB_SUCCESS!=rval)
    return 1;

  return 0; // no error
}

ErrCode iMOAB_SynchronizeTags(iMOAB_AppID pid, int * num_tag, int * tag_indices, int * ent_type)
{
#ifdef MOAB_HAVE_MPI
  appData & data = context.appDatas[*pid];
  Range ent_exchange;
  std::vector<Tag> tags;
  for (int i = 0; i<* num_tag; i++)
  {
    if (tag_indices[i]<0 || tag_indices[i]>= (int)data.tagList.size())
      return 1 ; // error in tag index
    tags.push_back( data.tagList[tag_indices[i]]);
  }
  if (* ent_type==0)
    ent_exchange = data.all_verts;
  else if (*ent_type ==1 )
    ent_exchange = data.primary_elems;
  else
    return 1; // unexpected type

  ParallelComm * pco = context.pcomms[*pid];

  ErrorCode rval = pco->exchange_tags(tags, tags, ent_exchange);
  if (rval!=MB_SUCCESS)
    return 1;
#else
  /* do nothing if serial */
  // just silence the warning
  // do not call sync tags in serial!
  int k = *pid+ *num_tag + *tag_indices + *ent_type; k++;
#endif

  return 0;
}

ErrCode iMOAB_GetNeighborElements(iMOAB_AppID pid, iMOAB_LocalID * local_index, int* num_adjacent_elements, iMOAB_LocalID* adjacent_element_IDs)
{
  //; one neighbor for each subentity of dimension-1
  MeshTopoUtil mtu(context.MBI);
  appData & data = context.appDatas[*pid];
  EntityHandle eh = data.primary_elems[*local_index];
  Range adjs;
  ErrorCode rval = mtu.get_bridge_adjacencies(eh, data.dimension-1, data.dimension, adjs);
  if (rval!=MB_SUCCESS)
    return 1;
  if (* num_adjacent_elements<(int)adjs.size())
    return 1; // not dimensioned correctly
  *num_adjacent_elements=(int)adjs.size();
  for (int i=0; i<* num_adjacent_elements; i++)
  {
    adjacent_element_IDs[i] = data.primary_elems.index(adjs[i]);
  }

  return 0;
}
#if 0

ErrCode iMOAB_GetNeighborVertices(iMOAB_AppID pid, iMOAB_LocalID* local_vertex_ID, int* num_adjacent_vertices, iMOAB_LocalID* adjacent_vertex_IDs)
{
  return 0;
}
#endif

ErrCode iMOAB_CreateVertices( iMOAB_AppID pid, int * coords_len, int *dim, double * coordinates )
{
  appData & data = context.appDatas[*pid];
  if (!data.local_verts.empty() ) // we should have no vertices in the app
    return 1;
  int nverts = *coords_len / *dim;

  ErrorCode rval = context.MBI->create_vertices(coordinates, nverts, data.local_verts);
  if (rval!=MB_SUCCESS)
    return 1;

  rval = context.MBI->add_entities( data.file_set, data.local_verts);
  if (rval!=MB_SUCCESS)
    return 1;

  // also add the vertices to the all_verts range
  data.all_verts.merge(data.local_verts);
  return 0;
}

ErrCode iMOAB_CreateElements( iMOAB_AppID pid, int *num_elem, int *type,  int *num_nodes_per_element,  int * connectivity ,
       int *block_ID)
{
  // Create elements
  appData & data = context.appDatas[*pid];

  ReadUtilIface *read_iface;
  ErrorCode rval = context.MBI->query_interface(read_iface);
  if (rval!=MB_SUCCESS)
    return 1;

  EntityType mbtype = (EntityType)(*type);
  EntityHandle actual_start_handle;
  EntityHandle* array=NULL;
  rval = read_iface->get_element_connect(*num_elem,
                                         *num_nodes_per_element,
                                         mbtype,
                                         1,
                                          actual_start_handle,
                                          array );

  if (rval!=MB_SUCCESS)
    return 1;

  // fill up with actual connectivity from input; assume the vertices are in order, and start vertex is
  // the first in the current data vertex range
  EntityHandle firstVertex = data.local_verts[0];

  for (int j=0; j<*num_elem * (*num_nodes_per_element); j++)
    array[j]=connectivity[j]+firstVertex-1; // assumes connectivity uses 1 based array (from fortran, mostly)

  Range new_elems(actual_start_handle, actual_start_handle+ *num_elem -1);

  rval = context.MBI->add_entities( data.file_set, new_elems);
  if (rval!=MB_SUCCESS)
    return 1;

  data.primary_elems.merge(new_elems);

  // organize all new elements in block, with the given block ID; if the block set is not existing, create  a
  // new mesh set;
  Range sets;
  int set_no = *block_ID;
  const void *setno_ptr = &set_no;
  rval = context.MBI->get_entities_by_type_and_tag(data.file_set, MBENTITYSET,
      &context.material_tag, &setno_ptr, 1, sets);
  EntityHandle block_set;
  if (MB_FAILURE == rval || sets.empty()) {
    // create a new set, with this block ID
    rval = context.MBI->create_meshset(MESHSET_SET, block_set );
    if (MB_FAILURE == rval)
      return 1; // failure
    rval = context.MBI->tag_set_data(context.material_tag, &block_set, 1, &set_no);
    if (MB_FAILURE == rval)
      return 1; // failure
    // add the material set to file set
    rval = context.MBI->add_entities(data.file_set, &block_set, 1);
    if (MB_FAILURE == rval)
      return 1; // failure
  }
  else
    block_set = sets[0]; // first set is the one we want

  /// add the new ents to the clock set
  rval = context.MBI->add_entities(block_set, new_elems);
  if (MB_FAILURE == rval)
    return 1; // failure

  return 0;
}

// this makes sense only for parallel runs
ErrCode iMOAB_ResolveSharedEntities(  iMOAB_AppID pid, int *num_verts, int * marker )
{

#ifdef MOAB_HAVE_MPI
  appData & data = context.appDatas[*pid];
  ParallelComm * pco = context.pcomms[*pid];

  // create an integer tag for resolving ; maybe it can be a long tag in the future
  // (more than 2 B vertices;)
  int dum_id = 0;
  Tag stag;
  ErrorCode rval = context.MBI->tag_get_handle("__sharedmarker", 1,  MB_TYPE_INTEGER, stag,
      MB_TAG_CREAT | MB_TAG_DENSE, &dum_id);
  if (rval!=MB_SUCCESS)
    return 1;
  if (*num_verts > (int)data.local_verts.size())
    return 1; // we are not setting the size
  rval = context.MBI->tag_set_data(stag, data.local_verts, (void*)marker); // assumes integer tag
  EntityHandle cset = data.file_set;
  rval = pco->resolve_shared_ents(cset, -1, -1, &stag);
  if (rval!=MB_SUCCESS)
    return 1;

  // provide partition tag equal to rank
  Tag part_tag;
  dum_id = -1;
  rval = context.MBI->tag_get_handle("PARALLEL_PARTITION", 1, MB_TYPE_INTEGER,
                               part_tag, MB_TAG_CREAT | MB_TAG_SPARSE, &dum_id);
  if (rval!=MB_SUCCESS)
    return 1;
  int rank = pco->rank();
  rval = context.MBI->tag_set_data(part_tag, &cset, 1, &rank);
  if (rval!=MB_SUCCESS)
    return 1;

#endif
  return 0;
}

// this assumes that this was not called before
ErrCode iMOAB_DetermineGhostEntities(  iMOAB_AppID pid, int * ghost_dim, int *num_ghost_layers, int * bridge_dim )
{
  if (*num_ghost_layers <= 0)
    return 0; // nothing to do
#ifdef MOAB_HAVE_MPI
  appData & data = context.appDatas[*pid];
  ParallelComm * pco = context.pcomms[*pid];

  int addl_ents=0; //maybe we should be passing this too; most of the time we do not need additional ents
  ErrorCode rval = pco->exchange_ghost_cells(*ghost_dim, *bridge_dim,
      *num_ghost_layers, addl_ents, true, true, &data.file_set); // collective call

  if (rval!=MB_SUCCESS)
    return 1;

  // now re-establish all mesh info; will reconstruct mesh info, based solely on what is in the file set
  int rc=iMOAB_UpdateMeshInfo(pid);
  return rc;
#endif
  return 0;
}

ErrCode iMOAB_SetGlobalInfo(iMOAB_AppID pid, int * num_global_verts, int * num_global_elems)
{
  appData & data = context.appDatas[*pid];
  data.num_global_vertices = *num_global_verts;
  data.num_global_elements = *num_global_elems;
  return 0;
}

ErrCode iMOAB_GetGlobalInfo(iMOAB_AppID pid, int * num_global_verts, int * num_global_elems)
{
  appData & data = context.appDatas[*pid];
  if (NULL != num_global_verts) *num_global_verts = data.num_global_vertices;
  if (NULL != num_global_elems) *num_global_elems = data.num_global_elements;
  return 0;
}


#ifdef MOAB_HAVE_MPI

ErrCode iMOAB_SendMesh(iMOAB_AppID pid, MPI_Comm * global, MPI_Group * receivingGroup, int * rcompid)
{
  //appData & data = context.appDatas[*pid];
  ParallelComm * pco = context.pcomms[*pid];

  MPI_Comm sender = pco->comm(); // the sender comm is obtained from parallel comm in moab;
                                 // no need to pass it along
  // first see what are the processors in each group; get the sender group too, from the sender communicator
  MPI_Group senderGroup;
  int ierr= MPI_Comm_group(sender, &senderGroup);
  if (ierr!=0)
    return 1;
  // instantiate the par comm graph
  // ParCommGraph::ParCommGraph(MPI_Comm joincomm, MPI_Group group1, MPI_Group group2, int coid1, int coid2)
  ParCommGraph * cgraph = new ParCommGraph(*global, senderGroup, *receivingGroup, context.appDatas[*pid].external_id, *rcompid);
  // we should search if we have another pcomm with the same comp ids in the list already
  // sort of check existing comm graphs in the list context.appDatas[*pid].pgraph
  context.appDatas[*pid].pgraph.push_back(cgraph);

  int sender_rank=-1;
  MPI_Comm_rank(sender, &sender_rank);

  // decide how to distribute elements to each processor
  // now, get the entities on local processor, and pack them into a buffer for various processors
  // we will do trivial partition: first get the total number of elements from "sender"
  std::vector<int> number_elems_per_part;
  // how to distribute local elements to receiving tasks?
  // trivial partition: compute first the total number of elements need to be sent
  Range & owned = context.appDatas[*pid].owned_elems;

  int local_owned_elem = (int) owned.size();
  int size = pco->size();
  int rank = pco->rank();
  number_elems_per_part.resize(size); //
  number_elems_per_part[rank] = local_owned_elem;
#if (MPI_VERSION >= 2)
      // Use "in place" option
  ierr = MPI_Allgather(MPI_IN_PLACE, 0, MPI_DATATYPE_NULL,
                              &number_elems_per_part[0], 1, MPI_INTEGER,
                              sender);
#else
  {
    std::vector<int> all_tmp(size);
    ierr = MPI_Allgather(&number_elems_per_part[rank], 1, MPI_INTEGER,
                            &all_tmp[0], 1, MPI_INTEGER,
                            sender);
    number_elems_per_part = all_tmp;
  }
#endif
  if (ierr!=0)
    return 1;

  int local_sender_rank =-1;
  ierr = MPI_Group_rank(senderGroup, &local_sender_rank);
  if (ierr!=0)
    return 1;

  // every sender computes the trivial partition, it is cheap, and we need to send it anyway to each sender
  ErrorCode rval = cgraph->compute_trivial_partition(number_elems_per_part);
  if (MB_SUCCESS!=rval)
	  return 1;

  if ( cgraph->is_root_sender())
  {
    // will need to build a communication graph, because each sender knows now to which receiver to send data
    // the receivers need to post receives for each sender that will send data to them
    // will need to gather on rank 0 on the sender comm, global ranks of sender with receivers to send
    // build communication matrix, each receiver will receive from what sender

    std::vector<int> packed_recv_array;
    rval = cgraph->pack_receivers_graph(packed_recv_array );
    if (MB_SUCCESS!=rval) return 1;

    int size_pack_array = (int) packed_recv_array.size();
    /// use tag 10 to send size and tag 20 to send the packed array
    ierr = MPI_Send(&size_pack_array, 1, MPI_INT, cgraph->receiver(0), 10, *global); // we have to use global communicator
    if (ierr!=0) return 1;
    ierr = MPI_Send(&packed_recv_array[0], size_pack_array, MPI_INT, cgraph->receiver(0), 20, *global); // we have to use global communicator
    if (ierr!=0) return 1;
  }

  std::map<int, Range> split_ranges;
  rval = cgraph->split_owned_range (sender_rank, owned, split_ranges);
  if (rval!=MB_SUCCESS) return 1;

  for (std::map<int, Range>::iterator it=split_ranges.begin(); it!=split_ranges.end(); it++)
  {
    int receiver_proc = it->first;
    Range ents = it->second;

    // add necessary vertices too
    Range verts;
    rval = context.MBI->get_adjacencies(ents, 0, false, verts, Interface::UNION);
    if (rval!=MB_SUCCESS) return 1;
    ents.merge(verts);
    ParallelComm::Buffer buffer(ParallelComm::INITIAL_BUFF_SIZE);
    buffer.reset_ptr(sizeof(int));
    rval = pco->pack_buffer(ents, false, true, false, -1, &buffer); if (rval!=MB_SUCCESS) return 1;
    int size_pack = buffer.get_current_size();

    ierr = MPI_Send(&size_pack, 1, MPI_INT, receiver_proc, 1, *global); // we have to use global communicator
    if (ierr!=0) return 1;

    ierr = MPI_Send(buffer.mem_ptr, size_pack, MPI_CHAR, receiver_proc, 2, *global); // we have to use global communicator
    if (ierr!=0) return 1;

  }
  return 0;
}
ErrCode iMOAB_ReceiveMesh(iMOAB_AppID pid, MPI_Comm * global, MPI_Group * sendingGroup,
    int * scompid)
{
  appData & data = context.appDatas[*pid];
  ParallelComm * pco = context.pcomms[*pid];
  MPI_Comm receive = pco->comm();
  EntityHandle local_set = data.file_set;
  ErrorCode rval;

  // first see what are the processors in each group; get the sender group too, from the sender communicator
  MPI_Group receiverGroup;
  int ierr= MPI_Comm_group(receive, &receiverGroup);
  if (ierr!=0)
    return 1;

  // instantiate the par comm graph
  ParCommGraph * cgraph = new ParCommGraph(*global, *sendingGroup, receiverGroup, *scompid, context.appDatas[*pid].external_id);
  // TODO we should search if we have another pcomm with the same comp ids in the list already
  // sort of check existing comm graphs in the list context.appDatas[*pid].pgraph
  context.appDatas[*pid].pgraph.push_back(cgraph);

  int receiver_rank=-1;
  MPI_Comm_rank(receive, &receiver_rank);

  // first, receive from sender_rank 0, the communication graph (matrix), so each receiver
  // knows what data to expect
  int size_pack_array;
  std::vector<int> pack_array;
  MPI_Status status;
  if (0==receiver_rank)
  {
    ierr = MPI_Recv (&size_pack_array, 1, MPI_INT, cgraph->sender(0), 10, *global, &status);
    if (0!=ierr) return 1;
#ifdef VERBOSE
    std::cout <<" receive comm graph size: " << size_pack_array << "\n";
#endif
    pack_array.resize (size_pack_array);
    ierr = MPI_Recv (&pack_array[0], size_pack_array, MPI_INT, cgraph->sender(0), 20, *global, &status);
    if (0!=ierr) return 1;
#ifdef VERBOSE
    std::cout <<" receive comm graph " ;
    for (int k=0; k<(int)pack_array.size(); k++)
      std::cout << " " << pack_array[k];
    std::cout <<"\n";
#endif
  }

  // now broadcast this whole array to all receivers, so they know what to expect
  MPI_Bcast(&size_pack_array, 1, MPI_INT, 0, receive);
  pack_array.resize(size_pack_array);
  MPI_Bcast(&pack_array[0], size_pack_array, MPI_INT, 0, receive);
  // now identify for current task, where are we getting data from

  // senders across for the current receiver
  int current_receiver = cgraph->receiver(receiver_rank);

  std::vector<int> senders_local;
  size_t n=0;
  while(n < pack_array.size())
  {
    if (current_receiver == pack_array[n])
    {
      for (int j=0; j < pack_array[n+1]; j++)
        senders_local.push_back(pack_array[n+2+j]);
      break;
    }
    n = n + 2 + pack_array[n+1];
  }
  if (!senders_local.empty())
  {
#ifdef VERBOSE
    std:: cout << " receiver " << current_receiver << " at rank " <<
        receiver_rank << " will receive from " << senders_local.size() << " tasks: ";
    for (int k=0; k<(int)senders_local.size(); k++)
      std::cout << " " << senders_local[k];
    std::cout<<"\n";
#endif
    for (size_t k=0; k< senders_local.size(); k++)
    {
      int sender = senders_local[k]; // first receive the size of the buffer

      int size_pack;
      ierr = MPI_Recv (&size_pack, 1, MPI_INT, sender, 1, *global, &status);
      if (0!=ierr) return 1;
      // now resize the buffer, then receive it
      ParallelComm::Buffer buff(size_pack);

      ierr = MPI_Recv (buff.mem_ptr, size_pack, MPI_CHAR, sender, 2, *global, &status);
      if (0!=ierr) return 1;
      // now unpack the buffer we just received
      Range entities;
      std::vector<std::vector<EntityHandle> > L1hloc, L1hrem;
      std::vector<std::vector<int> > L1p;
      std::vector<EntityHandle> L2hloc, L2hrem;
      std::vector<unsigned int> L2p;
      buff.reset_ptr(sizeof(int));
      std::vector<EntityHandle> entities_vec(entities.size());
      std::copy(entities.begin(), entities.end(), entities_vec.begin());
      rval = pco->unpack_buffer(buff.buff_ptr, false, -1, -1, L1hloc, L1hrem, L1p, L2hloc,
                                  L2hrem, L2p, entities_vec);
      if (MB_SUCCESS!= rval) return 1;

      std::copy(entities_vec.begin(), entities_vec.end(), range_inserter(entities));
      // we have to add them to the local set
      rval = context.MBI->add_entities(local_set, entities);
      if (MB_SUCCESS!= rval) return 1;

#ifdef VERBOSE
      std::ostringstream partial_outFile;

      partial_outFile <<"part_send_" <<sender<<"."<< "recv"<< current_receiver <<".vtk";

        // the mesh contains ghosts too, but they are not part of mat/neumann set
        // write in serial the file, to see what tags are missing
      std::cout<< " writing from receiver " << current_receiver << " at rank " <<
        receiver_rank << " from sender " <<  sender << " entities: " << entities.size() << std::endl;
      rval = context.MBI->write_file(partial_outFile.str().c_str(), 0, 0, &local_set, 1); // everything on local set received
      if (MB_SUCCESS!= rval) return 1;
#endif
    }

  }
  // in order for the merging to work, we need to be sure that the adjacencies are updated (created)
  Core* this_core = dynamic_cast<Core*>(context.MBI);
  if (this_core && !this_core->a_entity_factory()->vert_elem_adjacencies())
    this_core->a_entity_factory()->create_vert_elem_adjacencies();

  // after we are done, we could merge vertices that come from different senders, but
  // have the same global id
  Tag idtag;
  rval = context.MBI->tag_get_handle("GLOBAL_ID", idtag);
  if (MB_SUCCESS != rval) return 1;

  if ((int)senders_local.size()>=2) // need to remove duplicate vertices
                                    // that might come from different senders
  {
    Range local_ents;
    rval = context.MBI->get_entities_by_handle(local_set, local_ents);
    if (MB_SUCCESS != rval) return 1;
    Range local_verts = local_ents.subset_by_type(MBVERTEX);
    Range local_elems = subtract(local_ents, local_verts);

    // remove from local set the vertices
    rval = context.MBI->remove_entities(local_set, local_verts);
    if (MB_SUCCESS!= rval) return 1;
#ifdef VERBOSE
    std::cout << "current_receiver " << current_receiver << " local verts: " << local_verts.size() << "\n";
#endif
    MergeMesh mm(context.MBI);
    rval = mm.merge_using_integer_tag(local_verts, idtag);
    if (MB_SUCCESS != rval) return 1;

    Range new_verts; // local elems are local entities without vertices
    rval = context.MBI->get_connectivity(local_elems, new_verts);
    if (MB_SUCCESS!= rval) return 1;
#ifdef VERBOSE
    std::cout << "after merging: new verts: " << new_verts.size() << "\n";
#endif
    rval = context.MBI->add_entities(local_set, new_verts);
    if (MB_SUCCESS!= rval) return 1;
  }

  // still need to resolve shared entities (in this case, vertices )
  rval = pco->resolve_shared_ents(local_set, -1, -1, &idtag);
  if (rval != MB_SUCCESS) return 1;

  // populate the mesh with current data info
  ierr = iMOAB_UpdateMeshInfo(pid);
  if (0!=ierr) return 1;

  return 0;
}
#endif

#ifdef __cplusplus
}
#endif
