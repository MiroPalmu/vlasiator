#include "cpu_1d_ppm_nonuniform.hpp"
//#include "cpu_1d_ppm_nonuniform_conserving.hpp"
#include "vec.h"
#include "../grid.h"
#include "../object_wrapper.h"
#include "../memoryallocation.h"
#include "cpu_trans_map_amr.hpp"
#include "cpu_trans_map.hpp"

// use DCCRG version Nov 8th 2018 01482cfba8

using namespace std;
using namespace spatial_cell;

// indices in padded source block, which is of type Vec with VECL
// element sin each vector. b_k is the block index in z direction in
// ordinary space [- VLASOV_STENCIL_WIDTH to VLASOV_STENCIL_WIDTH],
// i,j,k are the cell ids inside on block (i in vector elements).
// Vectors with same i,j,k coordinates, but in different spatial cells, are consequtive
#define i_trans_ps_blockv(planeVectorIndex, planeIndex, blockIndex) ( (blockIndex) + VLASOV_STENCIL_WIDTH  +  ( (planeVectorIndex) + (planeIndex) * VEC_PER_PLANE ) * ( 1 + 2 * VLASOV_STENCIL_WIDTH)  )

// indices in padded target block, which is of type Vec with VECL
// element sin each vector. b_k is the block index in z direction in
// ordinary space, i,j,k are the cell ids inside on block (i in vector
// elements).
#define i_trans_pt_blockv(planeVectorIndex, planeIndex, blockIndex)  ( planeVectorIndex + planeIndex * VEC_PER_PLANE + (blockIndex + 1) * VEC_PER_BLOCK)

#define i_trans_ps_blockv_pencil(planeVectorIndex, planeIndex, blockIndex, lengthOfPencil) ( (blockIndex) + VLASOV_STENCIL_WIDTH  +  ( (planeVectorIndex) + (planeIndex) * VEC_PER_PLANE ) * ( lengthOfPencil + 2 * VLASOV_STENCIL_WIDTH) )


/* Get the one-dimensional neighborhood index for a given direction and neighborhood size.
 * 
 * @param dimension spatial dimension of neighborhood
 * @param stencil neighborhood size in cells
 * @return neighborhood index that can be passed to DCCRG functions
 */
int getNeighborhood(const uint dimension, const uint stencil) {

   int neighborhood = 0;

   if (stencil == 1) {
      switch (dimension) {
      case 0:
         neighborhood = VLASOV_SOLVER_TARGET_X_NEIGHBORHOOD_ID;
         break;
      case 1:
         neighborhood = VLASOV_SOLVER_TARGET_Y_NEIGHBORHOOD_ID;
         break;
      case 2:
         neighborhood = VLASOV_SOLVER_TARGET_Z_NEIGHBORHOOD_ID;
         break;
      }
   }

   if (stencil > 1) {
      switch (dimension) {
      case 0:
         neighborhood = VLASOV_SOLVER_X_NEIGHBORHOOD_ID;
         break;
      case 1:
         neighborhood = VLASOV_SOLVER_Y_NEIGHBORHOOD_ID;
         break;
      case 2:
         neighborhood = VLASOV_SOLVER_Z_NEIGHBORHOOD_ID;
         break;
      }
   }
   
   return neighborhood;
   
}

void flagSpatialCellsForAmrCommunication(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                                         const vector<CellID>& localPropagatedCells) {

   // Only flag/unflag cells if AMR is active
   if (mpiGrid.get_maximum_refinement_level()==0) return;

   // return if there's no cells to flag
   if(localPropagatedCells.size() == 0) {
      std::cerr<<"No cells!"<<std::endl;
      return;
   }

   for (int dimension=0; dimension<3; dimension++) {
      // These neighborhoods now include the AMR addition beyond the regular vlasov stencil
      int neighborhood = getNeighborhood(dimension,VLASOV_STENCIL_WIDTH);

      // Set flags: loop over local cells
#pragma omp parallel for
      for (uint i=0; i<localPropagatedCells.size(); i++) {
         CellID c = localPropagatedCells[i];
         SpatialCell *ccell = mpiGrid[c];
         if (!ccell) continue;

         // Translated cells also need to be included in order to communicate boundary cell VDFs.
         // Attempting to leave these out for the x or y dimensions also resulted in diffs.
         // if (!do_translate_cell(ccell)) continue;

         // Start with false
         ccell->SpatialCell::parameters[CellParams::AMR_TRANSLATE_COMM_X+dimension] = false;

         // In dimension, check iteratively if any neighbors up to VLASOV_STENCIL_WIDTH distance away are on a different process
         const auto* NbrPairs = mpiGrid.get_neighbors_of(c, neighborhood);

         // Create list of unique distances
         std::set< int > distancesplus;
         std::set< int > distancesminus;
         std::set<CellID> foundNeighborsP;
         std::set<CellID> foundNeighborsM;
         /** Using sets of cells as well, we should only get one distance per
             (potentially less refined) cell. This should result in safe behaviour
             as long as the neighborhood of a cell does not contain cells with a
             refinement level more than 1 level apart from the cell itself.
         */
         for (const auto nbrPair : *NbrPairs) {
            if(nbrPair.second[dimension] > 0) {
               if (foundNeighborsP.find(nbrPair.first) == foundNeighborsP.end()) {
                  distancesplus.insert(nbrPair.second[dimension]);
                  foundNeighborsP.insert(nbrPair.first);
               }
            }
            if(nbrPair.second[dimension] < 0) {
               if (foundNeighborsM.find(nbrPair.first) == foundNeighborsM.end()) {
                  distancesminus.insert(-nbrPair.second[dimension]);
                  foundNeighborsM.insert(nbrPair.first);
               }
            }
         }

         foundNeighborsP.clear();
         foundNeighborsM.clear();

         int iSrc = VLASOV_STENCIL_WIDTH - 1;
         // Iterate through positive distances for VLASOV_STENCIL_WIDTH elements starting from the smallest distance.
         for (auto it = distancesplus.begin(); it != distancesplus.end(); ++it) {
            if (ccell->SpatialCell::parameters[CellParams::AMR_TRANSLATE_COMM_X+dimension] == true) iSrc = -1;
            if (iSrc < 0) break; // found enough elements
            // Check all neighbors at distance *it
            for (const auto nbrPair : *NbrPairs) {
               SpatialCell *ncell = mpiGrid[nbrPair.first];
               if (!ncell) continue;
               int distanceInRefinedCells = nbrPair.second[dimension];
               if (distanceInRefinedCells == *it) {
                  if (foundNeighborsP.find(nbrPair.first) != foundNeighborsP.end()) continue;
                  foundNeighborsP.insert(nbrPair.first);
                  if (!mpiGrid.is_local(nbrPair.first)) {
                     ccell->SpatialCell::parameters[CellParams::AMR_TRANSLATE_COMM_X+dimension] = true;
                     break;
                  }
               }
            } // end loop over neighbors
            iSrc--;
         } // end loop over positive distances

         iSrc = VLASOV_STENCIL_WIDTH - 1;
         // Iterate through negtive distances for VLASOV_STENCIL_WIDTH elements starting from the smallest distance.
         for (auto it = distancesminus.begin(); it != distancesminus.end(); ++it) {
            if (ccell->SpatialCell::parameters[CellParams::AMR_TRANSLATE_COMM_X+dimension] == true) iSrc = -1;
            if (iSrc < 0) break; // found enough elements
            // Check all neighbors at distance *it
            for (const auto nbrPair : *NbrPairs) {
               SpatialCell *ncell = mpiGrid[nbrPair.first];
               if (!ncell) continue;
               int distanceInRefinedCells = -nbrPair.second[dimension];
               if (distanceInRefinedCells == *it) {
                  if (foundNeighborsM.find(nbrPair.first) != foundNeighborsM.end()) continue;
                  foundNeighborsM.insert(nbrPair.first);
                  if (!mpiGrid.is_local(nbrPair.first)) {
                     ccell->SpatialCell::parameters[CellParams::AMR_TRANSLATE_COMM_X+dimension] = true;
                     break;
                  }
               }
            } // end loop over neighbors
            iSrc--;
         } // end loop over negative distances
      } // end loop over local propagated cells
   } // end loop over dimensions
   return;
}

/* Get pointers to spatial cells that are considered source cells for a pencil.
 * Source cells are cells that the pencil reads data from to compute polynomial
 * fits that are used for propagation in the vlasov solver. All cells included
 * in the pencil + VLASOV_STENCIL_WIDTH cells on both ends are source cells.
 * Invalid cells are replaced by closest good cells.
 * Boundary cells are included.
 *
 * @param [in] mpiGrid DCCRG grid object
 * @param [in] pencils pencil data struct
 * @param [in] ipencil index of a pencil in the pencils data struct
 * @param [in] dimension spatial dimension
 * @param [out] sourceCells pointer to an array of pointers to SpatialCell objects for the source cells
 */
void computeSpatialSourceCellsForPencil(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                                        setOfPencils& pencils,
                                        const uint iPencil,
                                        const uint dimension,
                                        SpatialCell **sourceCells){

   // L = length of the pencil iPencil
   int L = pencils.lengthOfPencils[iPencil];
   vector<CellID> ids = pencils.getIds(iPencil);

   // These neighborhoods now include the AMR addition beyond the regular vlasov stencil
   int neighborhood = getNeighborhood(dimension,VLASOV_STENCIL_WIDTH);

   // Get pointers for each cell id of the pencil
   for (int i = 0; i < L; ++i) {
      sourceCells[i + VLASOV_STENCIL_WIDTH] = mpiGrid[ids[i]];
   }

   // Insert pointers for neighbors of ids.front() and ids.back()
   const auto* frontNbrPairs = mpiGrid.get_neighbors_of(ids.front(), neighborhood);
   const auto* backNbrPairs  = mpiGrid.get_neighbors_of(ids.back(),  neighborhood);

   // Create list of unique distances in the negative direction from the first cell in pencil
   std::set< int > distances;
   for (const auto nbrPair : *frontNbrPairs) {
      if(nbrPair.second[dimension] < 0) {
         // gather positive distance values
         distances.insert(-nbrPair.second[dimension]);
      }
   }

   int iSrc = VLASOV_STENCIL_WIDTH - 1;

   // Iterate through distances for VLASOV_STENCIL_WIDTH elements starting from the smallest distance.
   for (auto it = distances.begin(); it != distances.end(); ++it) {
      if (iSrc < 0) break; // found enough elements

      // Collect all neighbors at distance *it to a vector
      std::vector< CellID > neighbors;
      for (const auto nbrPair : *frontNbrPairs) {
         int distanceInRefinedCells = -nbrPair.second[dimension];
         if(distanceInRefinedCells == *it) neighbors.push_back(nbrPair.first);
      }
      // Get rid of duplicate neighbor cells at single distance
      neighbors.erase(unique(neighbors.begin(), neighbors.end()), neighbors.end());

      int refLvl = mpiGrid.get_refinement_level(ids.front());
      if (neighbors.size() == 1) {
         if (sourceCells[iSrc+1] == mpiGrid[neighbors.at(0)]) continue; // already found this cell for different distance         
         sourceCells[iSrc--] = mpiGrid[neighbors.at(0)];
      } else if ( pencils.path[iPencil][refLvl] < neighbors.size() ) {
         if (sourceCells[iSrc+1] == mpiGrid[neighbors.at(pencils.path[iPencil][refLvl])]) continue; // already found this cell for different distance (should not happen)
         sourceCells[iSrc--] = mpiGrid[neighbors.at(pencils.path[iPencil][refLvl])];
         // Code for alternate approach to verify that multiple neighbors are in correct ordering (z-y-x)
         // int ix=0,iy=0;
         // switch(dimension) {
         //    case 0:
         //       ix = 1;
         //       iy = 2;
         //       break;
         //    case 1:
         //       ix = 0;
         //       iy = 2;
         //       break;
         //    case 2:
         //       ix = 0;
         //       iy = 1;
         //       break;
         // }
         // bool accept = false;
         // std::array<double, 3> parentCoords = mpiGrid.get_center(ids.front());
         // for (CellID n : neighbors) {
         //    std::array<double, 3> myCoords = mpiGrid.get_center(n);
         //    switch (pencils.path[iPencil][refLvl]) {
         //       case 0:
         //          if (myCoords[ix] < parentCoords[ix] && myCoords[iy] < parentCoords[iy]) accept=true;
         //          break;
         //       case 1:
         //          if (myCoords[ix] > parentCoords[ix] && myCoords[iy] < parentCoords[iy]) accept=true;
         //             break;
         //       case 2:
         //          if (myCoords[ix] < parentCoords[ix] && myCoords[iy] > parentCoords[iy]) accept=true;
         //          break;
         //       case 3:
         //          if (myCoords[ix] > parentCoords[ix] && myCoords[iy] > parentCoords[iy]) accept=true;
         //          break;
         //    }
         //    if (accept) {
         //       sourceCells[iSrc--] = mpiGrid[n];
         //       break;
         //    }
         // }
      } else {
         std::cerr<<"error too few neighbors for path! "<<std::endl; 
      }
   }

   iSrc = L + VLASOV_STENCIL_WIDTH;
   distances.clear();
   // Create list of unique distances in the positive direction from the last cell in pencil
   for (const auto nbrPair : *backNbrPairs) {
      if(nbrPair.second[dimension] > 0) {
         distances.insert(nbrPair.second[dimension]);
      }
   }

   // Iterate through distances for VLASOV_STENCIL_WIDTH elements starting from the smallest distance.
   // Distances are positive here so smallest distance has smallest value.
   for (auto it = distances.begin(); it != distances.end(); ++it) {
      if (iSrc >= L+2*VLASOV_STENCIL_WIDTH) break; // Found enough cells

      // Collect all neighbors at distance *it to a vector
      std::vector< CellID > neighbors;
      for (const auto nbrPair : *backNbrPairs) {
         int distanceInRefinedCells = nbrPair.second[dimension];
         if(distanceInRefinedCells == *it) neighbors.push_back(nbrPair.first);
      }
      // Get rid of duplicate neighbor cells at single distance
      neighbors.erase(unique(neighbors.begin(), neighbors.end()), neighbors.end());

      int refLvl = mpiGrid.get_refinement_level(ids.back());
      if (neighbors.size() == 1) {
         if (sourceCells[iSrc-1] == mpiGrid[neighbors.at(0)]) continue; // already found this cell for different distance
         sourceCells[iSrc++] = mpiGrid[neighbors.at(0)];
      } else if ( pencils.path[iPencil][refLvl] < neighbors.size() ) {
         if (sourceCells[iSrc-1] == mpiGrid[neighbors.at(pencils.path[iPencil][refLvl])]) continue; // already found this cell for different distance (should not happen)
         sourceCells[iSrc++] = mpiGrid[neighbors.at(pencils.path[iPencil][refLvl])];
      } else {
         std::cerr<<"error too few neighbors for path!"<<std::endl;
      }
   }

   /*loop to negative side and replace all invalid cells with the closest good cell*/
   SpatialCell* lastGoodCell = mpiGrid[ids.front()];
   for(int i = VLASOV_STENCIL_WIDTH - 1; i >= 0 ;--i){
      if(sourceCells[i] == NULL || sourceCells[i]->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE)
         sourceCells[i] = lastGoodCell;
      else
         lastGoodCell = sourceCells[i];
   }
   /*loop to positive side and replace all invalid cells with the closest good cell*/
   lastGoodCell = mpiGrid[ids.back()];
   for(int i = VLASOV_STENCIL_WIDTH + L; i < (L + 2*VLASOV_STENCIL_WIDTH); ++i){
      if(sourceCells[i] == NULL || sourceCells[i]->sysBoundaryFlag == sysboundarytype::DO_NOT_COMPUTE)
         sourceCells[i] = lastGoodCell;
      else
         lastGoodCell = sourceCells[i];
   }
}


/* Get pointers to spatial cells that are considered target cells for all pencils.
 * Target cells are cells that the pencil writes data into after translation by
 * the vlasov solver. All cells included in the pencil + 1 cells on both ends 
 * are source cells. Boundary cells are not included.
 * Now uses get_face_neighbors_of().
 *
 * @param [in] mpiGrid DCCRG grid object
 * @param [in] pencils pencil data struct
 * @param [in] dimension spatial dimension
 * @param [out] targetCells pointer to an array of pointers to SpatialCell objects for the target cells
 *
 */
void computeSpatialTargetCellsForPencilsWithFaces(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                                         setOfPencils& pencils,
                                         const uint dimension,
                                         SpatialCell **targetCells){

   uint GID = 0;
   // Loop over pencils
   for(uint iPencil = 0; iPencil < pencils.N; iPencil++){
      int L = pencils.lengthOfPencils[iPencil];
      vector<CellID> ids = pencils.getIds(iPencil);

      // Get pointers for each cell id of the pencil
      for (int i = 0; i < L; ++i) {
         targetCells[GID + i + 1] = mpiGrid[ids[i]];
      }

      int refLvl;
      vector <CellID> frontNeighborIds;
      vector <CellID> backNeighborIds;
      const auto frontNeighbors = mpiGrid.get_face_neighbors_of(ids.front());
      if (frontNeighbors.size() > 0) {
         for (const auto nbr: frontNeighbors) {
            if(nbr.second == (-((int)dimension + 1))) {
               frontNeighborIds.push_back(nbr.first);
            }
         }
         refLvl = mpiGrid.get_refinement_level(ids.front());
         
         if (frontNeighborIds.size() == 0) {
            std::cerr<<"abort frontNeighborIds.size() == 0 at "<<ids.front()<<std::endl;
            for( const auto nbrPair: frontNeighbors ) {
               std::cerr<<ids.front()<<" dim "<<dimension<<" "<<nbrPair.first<<" "<<nbrPair.second<<std::endl;
            }
         }
         if (frontNeighborIds.size() == 1) {
            targetCells[GID] = mpiGrid[frontNeighborIds[0]];
         } else if ( pencils.path[iPencil][refLvl] < frontNeighborIds.size() ) {
            targetCells[GID] = mpiGrid[frontNeighborIds[pencils.path[iPencil][refLvl]]];
         }
      } else {
         std::cerr<<"error, found cell without any face neighbors"<<std::endl;
      }
      frontNeighborIds.clear();

      const auto backNeighbors = mpiGrid.get_face_neighbors_of(ids.back());
      if (backNeighbors.size() > 0) {
         for (const auto nbr: backNeighbors) {
            if(nbr.second == ((int)dimension + 1)) {
               backNeighborIds.push_back(nbr.first);
            }
         }
         refLvl = mpiGrid.get_refinement_level(ids.back());
         if (backNeighborIds.size() == 0) {
            std::cerr<<"abort backNeighborIds.size() == 0 at "<<ids.back()<<std::endl;
            for( const auto nbrPair: backNeighbors ) {
               std::cerr<<ids.back()<<" dim "<<dimension<<" "<<nbrPair.first<<" "<<nbrPair.second<<std::endl;
            }
         }
         if (backNeighborIds.size() == 1) {
            targetCells[GID + L + 1] = mpiGrid[backNeighborIds[0]];
         } else if ( pencils.path[iPencil][refLvl] < backNeighborIds.size() ) {
            targetCells[GID + L + 1] = mpiGrid[backNeighborIds[pencils.path[iPencil][refLvl]]];
         }
      } else {
         std::cerr<<"error, found cell without any face neighbors"<<std::endl;
      }
      backNeighborIds.clear();

      // Incerment global id by L + 2 ghost cells.
      GID += (L + 2);
   }

   // Remove any boundary cells from the list of valid targets
   for (uint i = 0; i < GID; ++i) {
      if (targetCells[i] && targetCells[i]->sysBoundaryFlag != sysboundarytype::NOT_SYSBOUNDARY ) {
         targetCells[i] = NULL;
      }
   }
}

/* Select one nearest neighbor of a cell on the + side in a given dimension. If the neighbor
 * has a higher level of refinement, a path variable is needed to make the selection.
 * Returns INVALID_CELLID if the nearest neighbor is not local to this process.
 * 
 * @param grid DCCRG grid object
 * @param id DCCRG cell id
 * @param dimension spatial dimension
 * @param path index of the desired face neighbor
 * @return neighbor DCCRG cell id of the neighbor
 */
CellID selectNeighbor(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry> &grid,
                      CellID id, int dimension = 0, uint path = 0) {

   //int neighborhood = getNeighborhood(dimension,1);
   //const auto* nbrPairs = grid.get_neighbors_of(id, neighborhood);
   
   vector < CellID > myNeighbors;
   CellID neighbor = INVALID_CELLID;
   
   // Iterate through neighbor ids in the positive direction of the chosen dimension,
   // select the neighbor indicated by path, if it is local to this process.
   const auto faceNbrs = grid.get_face_neighbors_of(id);
   for (const auto nbr : faceNbrs) {
     if (nbr.second == ((int)dimension + 1)) {
	 myNeighbors.push_back(nbr.first);
      }
   }
   
   if( myNeighbors.size() == 0 ) {
      return neighbor;
   }
   
   int neighborIndex = 0;
   if (myNeighbors.size() > 1) {
      neighborIndex = path;
   }
   
   if (grid.is_local(myNeighbors[neighborIndex])) {
     neighbor = myNeighbors[neighborIndex];
   }
   
   return neighbor;
}

/* Recursive function for building one-dimensional pencils to cover local DCCRG cells.
 * Starts from a given seedID and proceeds finding the nearest neighbor in the given dimension
 * and adding it to the pencil until no neighbors are found or an endId is met. When a higher
 * refinement level (ie. multiple nearest neighbors) is met, the pencil splits into four
 * copies to remain at a width of 1 cell. This is done by the function calling itself recursively
 * and passing as inputs the cells added so far. The cell selected by each copy of the function
 * at a split is stored in the path variable, the same path has to be followed if a refinement
 * level is encoutered multiple times.
 *
 * @param [in] grid DCCRG grid object
 * @param [out] pencils Pencil data struct
 * @param [in] seedId DCCRG cell id where we start building the pencil. 
 *             The pencil will continue in the + direction in the given dimension until an end condition is met
 * @param [in] dimension Spatial dimension
 * @param [in] path Integer value that determines which neighbor is added to the pencil when a higher refinement level is met
 * @param [in] endIds Prescribed end conditions for the pencil. If any of these cell ids is about to be added to the pencil,
 *             the builder terminates.
 */
setOfPencils buildPencilsWithNeighbors( const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry> &grid, 
					setOfPencils &pencils, const CellID seedId,
					vector<CellID> ids, const uint dimension, 
					vector<uint> path, const vector<CellID> &endIds) {

   const bool debug = false;
   CellID nextNeighbor;
   CellID id = seedId;
   int startingRefLvl = grid.get_refinement_level(id);
   bool periodic = false;
   // If this is a new pencil (instead of being a result of a pencil being split
   if( ids.size() == 0 )
      ids.push_back(seedId);

   // If the cell where we start is refined, we need to figure out which path
   // to follow in future refined cells. This is a bit hacky but we have to
   // use the order or the children of the parent cell to figure out which
   // corner we are in.

   std::array<double, 3> coordinates = grid.get_center(seedId);
   int startingPathSize = path.size();
   auto it = path.end();
   if( startingRefLvl > startingPathSize ) {

      CellID myId = seedId;
      
      for ( int i = path.size(); i < startingRefLvl; ++i) {

         //CellID parentId = grid.mapping.get_parent(myId);
         CellID parentId = grid.get_parent(myId);
         
         auto myCoords = grid.get_center(myId);
         auto parentCoords = grid.get_center(parentId);

         int ix=0,iy=0;

         switch(dimension) {
         case 0:
            ix = 1;
            iy = 2;
            break;
         case 1:
            ix = 0;
            iy = 2;
            break;
         case 2:
            ix = 0;
            iy = 1;
            break;
         }
         //int ix = (dimension + 1) % 3; // incorrect for DCCRG
         //int iy = (dimension + 2) % 3;

         int step = -1;
         
         if        (myCoords[ix] < parentCoords[ix] && myCoords[iy] < parentCoords[iy]) {
            step = 0;
         } else if (myCoords[ix] > parentCoords[ix] && myCoords[iy] < parentCoords[iy]) {
            step = 1;
         } else if (myCoords[ix] < parentCoords[ix] && myCoords[iy] > parentCoords[iy]) {
            step = 2;
         } else if (myCoords[ix] > parentCoords[ix] && myCoords[iy] > parentCoords[iy]) {
            step = 3;
         }

         it = path.insert(it, step);

         myId = parentId;
      }
   }
   
   while (id != INVALID_CELLID) {

      periodic = false;
      bool neighborExists = false;
      int refLvl = 0;
      
      // Find the refinement level in the neighboring cell. Check all possible neighbors
      // in case some of them are remote.
      for (int tmpPath = 0; tmpPath < 4; ++tmpPath) {
         nextNeighbor = selectNeighbor(grid,id,dimension,tmpPath);
         if(nextNeighbor != INVALID_CELLID) {
            refLvl = max(refLvl,grid.get_refinement_level(nextNeighbor));
            neighborExists = true;
         }
      }
         
      // If there are no neighbors, we can stop.
      if (!neighborExists)
         break;   

      if (refLvl > 0) {
    
         // If we have encountered this refinement level before and stored
         // the path this builder follows, we will just take the same path
         // again.
         if ( static_cast<int>(path.size()) >= refLvl ) {
      
            if(debug) {
               std::cout << "I am cell " << id << ". ";
               std::cout << "I have seen refinement level " << refLvl << " before. Path is ";
               for (auto k = path.begin(); k != path.end(); ++k)
                  std::cout << *k << " ";
               std::cout << std::endl;
            }
	
            nextNeighbor = selectNeighbor(grid,id,dimension,path[refLvl - 1]);      
	    coordinates = grid.get_center(nextNeighbor);

         } else {
	
            if(debug) {
               std::cout << "I am cell " << id << ". ";
               std::cout << "I have NOT seen refinement level " << refLvl << " before. Path is ";
               for (auto k = path.begin(); k != path.end(); ++k)
                  std::cout << *k << ' ';
               std::cout << std::endl;
            }
	
            // New refinement level, create a path through each neighbor cell
            for ( uint i : {0,1,2,3} ) {
	  
               vector < uint > myPath = path;
               myPath.push_back(i);
	  
               nextNeighbor = selectNeighbor(grid,id,dimension,myPath.back());
	  
               if ( i == 3 ) {
	    
                  // This builder continues with neighbor 3
                  path = myPath;
		  coordinates = grid.get_center(nextNeighbor);

               } else {
	    
                  // Spawn new builders for neighbors 0,1,2
                  buildPencilsWithNeighbors(grid,pencils,id,ids,dimension,myPath,endIds);
	    
               }
	  
            }
	
         }

      } else {
         if(debug) {
            std::cout << "I am cell " << id << ". ";
            std::cout << " This pencil has reached refinement level 0." << std::endl;
         }
      }// Closes if (refLvl == 0)

      // If we found a neighbor, add it to the list of ids for this pencil.
      if(nextNeighbor != INVALID_CELLID) {
         if (debug) {
            std::cout << " Next neighbor is " << nextNeighbor << "." << std::endl;
         }

         if ( std::any_of(endIds.begin(), endIds.end(), [nextNeighbor](uint i){return i == nextNeighbor;}) ||
              !do_translate_cell(grid[nextNeighbor])) {
            
            nextNeighbor = INVALID_CELLID;
         } else {
            ids.push_back(nextNeighbor);
         }
      }
      
      id = nextNeighbor;
   } // Closes while loop

   // Get the x,y - coordinates of the pencil (in the direction perpendicular to the pencil)
   double x,y;
   int ix=0,iy=0;

   switch(dimension) {
   case 0:
      ix = 1;
      iy = 2;
      break;
   case 1:
      ix = 0;
      iy = 2;
      break;
   case 2:
      ix = 0;
      iy = 1;
      break;
   }
   //ix = (dimension + 1) % 3; // incorrect for DCCRG
   //iy = (dimension + 2) % 3;
      
   x = coordinates[ix];
   y = coordinates[iy];

   pencils.addPencil(ids,x,y,periodic,path);
   
   // TODO why do we have both return value and the argument modified in place? Could be made consistent.
   return pencils;
  
}

bool check_skip_remapping(Vec* values) {
   for (int index=0; index<2*VLASOV_STENCIL_WIDTH+1; ++index) {
      if (horizontal_or(values[index] > Vec(0))) return false;
   }
   return true;
}
/* Propagate a given velocity block in all spatial cells of a pencil by a time step dt using a PPM reconstruction.
 *
 * @param dz Width of spatial cells in the direction of the pencil, vector datatype
 * @param values Density values of the block, vector datatype
 * @param dimension Satial dimension
 * @param blockGID Global ID of the velocity block.
 * @param dt Time step
 * @param vmesh Velocity mesh object
 * @param lengthOfPencil Number of cells in the pencil
 */
void propagatePencil(
   Vec* dz,
   Vec* values,
   Vec* targetValues, // thread-owned aligned-allocated
   const uint dimension,
   const uint blockGID,
   const Realv dt,
   const vmesh::VelocityMesh<vmesh::GlobalID,vmesh::LocalID> &vmesh,
   const uint lengthOfPencil,
   const Realv threshold
) {
   // Get velocity data from vmesh that we need later to calculate the translation
   velocity_block_indices_t block_indices;
   uint8_t refLevel;
   vmesh.getIndices(blockGID,refLevel, block_indices[0], block_indices[1], block_indices[2]);
   Realv dvz = vmesh.getCellSize(refLevel)[dimension];
   Realv vz_min = vmesh.getMeshMinLimits()[dimension];
   
   // Assuming 1 neighbor in the target array because of the CFL condition
   // In fact propagating to > 1 neighbor will give an error
   // Also defined in the calling function for the allocation of targetValues
   const uint nTargetNeighborsPerPencil = 1;

   
   for (uint i = 0; i < (lengthOfPencil + 2 * nTargetNeighborsPerPencil) * WID3 / VECL; i++) {
      
      // init target_values
      targetValues[i] = Vec(0.0);
      
   }
   
   // Go from 0 to length here to propagate all the cells in the pencil
   for (uint i = 0; i < lengthOfPencil; i++){
      
      // The source array is padded by VLASOV_STENCIL_WIDTH on both sides.
      uint i_source   = i + VLASOV_STENCIL_WIDTH;
      
      for (uint k = 0; k < WID; ++k) {

         const Realv cell_vz = (block_indices[dimension] * WID + k + 0.5) * dvz + vz_min; //cell centered velocity
         const Vec z_translation = cell_vz * dt / dz[i_source]; // how much it moved in time dt (reduced units)

         // Determine direction of translation
         // part of density goes here (cell index change along spatial direcion)
         Vecb positiveTranslationDirection = (z_translation > Vec(0.0));
         
         // Calculate normalized coordinates in current cell.
         // The coordinates (scaled units from 0 to 1) between which we will
         // integrate to put mass in the target  neighboring cell.
         // Normalize the coordinates to the origin cell. Then we scale with the difference
         // in volume between target and origin later when adding the integrated value.
         Vec z_1,z_2;
         z_1 = select(positiveTranslationDirection, 1.0 - z_translation, 0.0);
         z_2 = select(positiveTranslationDirection, 1.0, - z_translation);

         // if( horizontal_or(abs(z_1) > Vec(1.0)) || horizontal_or(abs(z_2) > Vec(1.0)) ) {
         //    std::cout << "Error, CFL condition violated\n";
         //    std::cout << "Exiting\n";
         //    std::exit(1);
         // }
         
         for (uint planeVector = 0; planeVector < VEC_PER_PLANE; planeVector++) {   

            // Check if all values are 0:
            if (check_skip_remapping(values + i_trans_ps_blockv_pencil(planeVector, k, i-VLASOV_STENCIL_WIDTH, lengthOfPencil))) continue;

            // Compute polynomial coefficients
            Vec a[3];
            // Dz: is a padded array, pointer can point to the beginning, i + VLASOV_STENCIL_WIDTH will get the right cell.
            // values: transpose function adds VLASOV_STENCIL_WIDTH to the block index, therefore we substract it here, then
            // i + VLASOV_STENCIL_WIDTH will point to the right cell. Complicated! Why! Sad! MVGA!
            compute_ppm_coeff_nonuniform(dz + i,
                                         values + i_trans_ps_blockv_pencil(planeVector, k, i-VLASOV_STENCIL_WIDTH, lengthOfPencil),
                                         h4, VLASOV_STENCIL_WIDTH, a, threshold);
            
            // Compute integral
            const Vec ngbr_target_density =
               z_2 * ( a[0] + z_2 * ( a[1] + z_2 * a[2] ) ) -
               z_1 * ( a[0] + z_1 * ( a[1] + z_1 * a[2] ) );
                                    
            // Store mapped density in two target cells
            // in the neighbor cell we will put this density
            targetValues[i_trans_pt_blockv(planeVector, k, i + 1)] += select( positiveTranslationDirection,
                                                                              ngbr_target_density
                                                                              * dz[i_source] / dz[i_source + 1],
                                                                              Vec(0.0));
            targetValues[i_trans_pt_blockv(planeVector, k, i - 1 )] += select(!positiveTranslationDirection,
                                                                              ngbr_target_density
                                                                              * dz[i_source] / dz[i_source - 1],
                                                                              Vec(0.0));
            
            // in the current original cells we will put the rest of the original density
            targetValues[i_trans_pt_blockv(planeVector, k, i)] += 
               values[i_trans_ps_blockv_pencil(planeVector, k, i, lengthOfPencil)] - ngbr_target_density;
         }
      }
   }
}

/* Determine which cells in the local DCCRG mesh should be starting points for pencils.
 * If a neighbor cell is non-local, across a periodic boundary, or in non-periodic boundary layer 1
 * then we use this cell as a seed for pencils
 *
 * @param [in] mpiGrid DCCRG grid object
 * @param [in] localPropagatedCells List of local cells that get propagated
 * ie. not boundary or DO_NOT_COMPUTE
 * @param [in] dimension Spatial dimension
 * @param [out] seedIds list of cell ids that will be starting points for pencils
 */
void getSeedIds(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                const vector<CellID> &localPropagatedCells,
                const uint dimension,
                vector<CellID> &seedIds) {

   const bool debug = false;
   int myRank;
   if (debug) MPI_Comm_rank(MPI_COMM_WORLD,&myRank);
   
   // These neighborhoods now include the AMR addition beyond the regular vlasov stencil
   int neighborhood = getNeighborhood(dimension,VLASOV_STENCIL_WIDTH);

#pragma omp parallel for
   for (uint i=0; i<localPropagatedCells.size(); i++) {
      CellID celli = localPropagatedCells[i];

      bool addToSeedIds = P::amrTransShortPencils;
      if (addToSeedIds) {
#pragma omp critical
         seedIds.push_back(celli);
         continue;
      }
      auto myIndices = mpiGrid.mapping.get_indices(celli);
      int myRefLevel;

      /* -----------------------------------------
         | A |   | B |   |_|_|_|_|   |   | C |   |
         |   |   |   |   | | | | |   |   |   |   |
         -----------------------------------------
         For optimal pencil generation, we need seedids at A, B, and C.
         A Is triggered in the first if-clause. Pencils starting from B
         will be split (but won't cause A to split), and pencils from
         C will be able to remain again un-split. These checks need to be done
         only if we aren't already at the maximum refinement level.
      */

      // First check negative face neighbors (A)
      // Returns all neighbors as (id, direction-dimension) pair pointers.
      for ( const auto faceNbrPair : mpiGrid.get_face_neighbors_of(celli) ) {
	 if ( faceNbrPair.second == -((int)dimension + 1) ) {
	    // Check that the neighbor is not across a periodic boundary by calculating
	    // the distance in indices between this cell and its neighbor.
	    auto nbrIndices = mpiGrid.mapping.get_indices(faceNbrPair.first);

	    // If a neighbor is non-local, across a periodic boundary, or in non-periodic boundary layer 1
	    // then we use this cell as a seed for pencils
	    if ( abs ( (int64_t)(myIndices[dimension] - nbrIndices[dimension]) ) >
		 pow(2,mpiGrid.get_maximum_refinement_level()) ||
		 !mpiGrid.is_local(faceNbrPair.first) ||
		 !do_translate_cell(mpiGrid[faceNbrPair.first]) ) {
               addToSeedIds = true;
               break;
	    }
         }
      } // finish check A
      if ( addToSeedIds ) {
#pragma omp critical
         seedIds.push_back(celli);
         continue;
      }
      myRefLevel = mpiGrid.get_refinement_level(celli);
      if (mpiGrid.get_maximum_refinement_level() == myRefLevel) continue;

      // Gather neighbours in neighbourhood stencil
      const auto* nbrPairs  = mpiGrid.get_neighbors_of(celli, neighborhood);
      // Create list of unique neighbour distances in both directions
      std::set< int > distancesplus;
      std::set< int > distancesminus;
      for (const auto nbrPair : *nbrPairs) {
         if(nbrPair.second[dimension] > 0) {
            distancesplus.insert(nbrPair.second[dimension]);
         }
         if(nbrPair.second[dimension] < 0) {
            // gather positive distance values
            distancesminus.insert(-nbrPair.second[dimension]);
         }
      }
      /* Proceed with B, checking if the next positive neighbour has the same refinement level as ccell, but the
         second neighbour a higher one. Iterate through positive distances for VLASOV_STENCIL_WIDTH elements
         starting from the smallest distance. */
      int iSrc = VLASOV_STENCIL_WIDTH-1;
      for (auto it = distancesplus.begin(); it != distancesplus.end(); ++it) {
         if (iSrc < 0) break; // found enough elements
         for (const auto nbrPair : *nbrPairs) {
            int distanceInRefinedCells = nbrPair.second[dimension];
            if(distanceInRefinedCells == *it) {
               // Break search if we are not at the final entry, and have different refinement level
               if (iSrc!=0 && mpiGrid.get_refinement_level(nbrPair.first)!=myRefLevel) {
                  iSrc = -1;
                  break;
               }
               // Flag as seed id if VLASOV_STENCIL_WIDTH positive neighbour is at higher refinement level
               if (iSrc==0 && mpiGrid.get_refinement_level(nbrPair.first)>myRefLevel) {
                  addToSeedIds = true;
                  break;
               }
            }
         }
         iSrc--;
      } // Finish B check

      if ( addToSeedIds ) {
#pragma omp critical
         seedIds.push_back(celli);
         continue;
      }
      /* Proceed with C, checking if the next two negative neighbours have the same refinement level as ccell, but the
         third neighbour a higher one. Iterate through negative distances for VLASOV_STENCIL_WIDTH+1 elements
         starting from the smallest distance. */
      iSrc = VLASOV_STENCIL_WIDTH;
      for (auto it = distancesminus.begin(); it != distancesminus.end(); ++it) {
         if (iSrc < 0) break; // found enough elements
         for (const auto nbrPair : *nbrPairs) {
            int distanceInRefinedCells = -nbrPair.second[dimension];
            if(distanceInRefinedCells == *it) {
               // Break search if we are not at the final entry, and have different refinement level
               if (iSrc!=0 && mpiGrid.get_refinement_level(nbrPair.first)!=myRefLevel) {
                  iSrc = -1;
                  break;
               }
               // Flag as seed id if VLASOV_STENCIL_WIDTH+1 positive neighbour is at higher refinement level
               if (iSrc==0 && mpiGrid.get_refinement_level(nbrPair.first)>myRefLevel) {
                  addToSeedIds = true;
                  break;
               }
            }
         }
         iSrc--;
      } // Finish C check

      if ( addToSeedIds ) {
#pragma omp critical
         seedIds.push_back(celli);
      }
   }

   if(debug) {
      cout << "Rank " << myRank << ", Seed ids are: ";
      for (const auto seedId : seedIds) {
         cout << seedId << " ";
      }
      cout << endl;
   }   
}




/* Copy the data to the temporary values array, so that the
 * dimensions are correctly swapped. Also, copy the same block for
 * then neighboring spatial cells (in the dimension). neighbors
 * generated with compute_spatial_neighbors_wboundcond).
 * Adapted from copy_trans_block_data to be suitable for use with
 * AMR and pencils. 
 *
 * This function must be thread-safe.
 *
 * @param source_neighbors Array containing the VLASOV_STENCIL_WIDTH closest 
 * spatial neighbors of this cell in the propagated dimension.
 * @param blockGID Global ID of the velocity block.
 * @param int lengthOfPencil Number of spatial cells in pencil
 * @param values Vector where loaded data is stored.
 * @param cellid_transpose
 * @param popID ID of the particle species.
 */
bool copy_trans_block_data_amr(
    SpatialCell** source_neighbors,
    const vmesh::GlobalID blockGID,
    int lengthOfPencil,
    Vec* values,
    const unsigned char* const cellid_transpose,
    const uint popID) { 

   // Allocate data pointer for all blocks in pencil. Pad on both ends by VLASOV_STENCIL_WIDTH
   Realf* blockDataPointer[lengthOfPencil + 2 * VLASOV_STENCIL_WIDTH];   

   int nonEmptyBlocks = 0;

   for (int b = -VLASOV_STENCIL_WIDTH; b < lengthOfPencil + VLASOV_STENCIL_WIDTH; b++) {
      // Get cell pointer and local block id
      SpatialCell* srcCell = source_neighbors[b + VLASOV_STENCIL_WIDTH];
         
      const vmesh::LocalID blockLID = srcCell->get_velocity_block_local_id(blockGID,popID);
      if (blockLID != srcCell->invalid_local_id()) {
         // Get data pointer
         blockDataPointer[b + VLASOV_STENCIL_WIDTH] = srcCell->get_data(blockLID,popID);
         nonEmptyBlocks++;
         // //prefetch storage pointers to L1
         // _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]), _MM_HINT_T0);
         // _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]) + 64, _MM_HINT_T0);
         // _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]) + 128, _MM_HINT_T0);
         // _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]) + 192, _MM_HINT_T0);
         // if(VPREC  == 8) {
         //   //prefetch storage pointers to L1
         //   _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]) + 256, _MM_HINT_T0);
         //   _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]) + 320, _MM_HINT_T0);
         //   _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]) + 384, _MM_HINT_T0);
         //   _mm_prefetch((char *)(blockDataPointer[b + VLASOV_STENCIL_WIDTH]) + 448, _MM_HINT_T0);
         // }
         
      } else {
         blockDataPointer[b + VLASOV_STENCIL_WIDTH] = NULL;
      }
   }
   
   if(nonEmptyBlocks == 0) {
      return false;
   }
   
   //  Copy volume averages of this block from all spatial cells:
   for (int b = -VLASOV_STENCIL_WIDTH; b < lengthOfPencil + VLASOV_STENCIL_WIDTH; b++) {
      if(blockDataPointer[b + VLASOV_STENCIL_WIDTH] != NULL) {
         Realf blockValues[WID3];
         const Realf* block_data = blockDataPointer[b + VLASOV_STENCIL_WIDTH];
         // Copy data to a temporary array and transpose values so that mapping is along k direction.
         // spatial source_neighbors already taken care of when
         // creating source_neighbors table. If a normal spatial cell does not
         // simply have the block, its value will be its null_block which
         // is fine. This null_block has a value of zero in data, and that
         // is thus the velocity space boundary
         for (uint i=0; i<WID3; ++i) {
            blockValues[i] = block_data[cellid_transpose[i]];
         }
         
         // now load values into the actual values table..
         uint offset =0;
         for (uint k=0; k<WID; k++) {
            for(uint planeVector = 0; planeVector < VEC_PER_PLANE; planeVector++){
               // store data, when reading data from data we swap dimensions 
               // using precomputed plane_index_to_id and cell_indices_to_id
               values[i_trans_ps_blockv_pencil(planeVector, k, b, lengthOfPencil)].load(blockValues + offset);
               offset += VECL;
            }
         }
      } else {
         for (uint k=0; k<WID; ++k) {
            for(uint planeVector = 0; planeVector < VEC_PER_PLANE; planeVector++) {
               values[i_trans_ps_blockv_pencil(planeVector, k, b, lengthOfPencil)] = Vec(0);
            }
         }
      }
   }
   return true;
}

/* Check whether the ghost cells around the pencil contain higher refinement than the pencil does.
 * If they do, the pencil must be split to match the finest refined ghost cell.
 *
 * @param mpiGrid DCCRG grid object
 * @param pencils Pencil data struct
 * @param dimension Spatial dimension
 */
void check_ghost_cells(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                       setOfPencils& pencils,
                       uint dimension) {

   const bool debug = false;
   int neighborhoodId = getNeighborhood(dimension,VLASOV_STENCIL_WIDTH);

   int myRank;
   if(debug) {
      MPI_Comm_rank(MPI_COMM_WORLD,&myRank);
   }
   
   std::vector<CellID> idsToSplit;

// Thread this loop here
#pragma omp parallel for   
   for (uint pencili = 0; pencili < pencils.N; ++pencili) {

      if(pencils.periodic[pencili]) continue;
         
      auto ids = pencils.getIds(pencili);

      // It is possible that the pencil has already been refined by the pencil building algorithm
      // and is on a higher refinement level than the refinement level of any of the cells it contains
      // due to e.g. process boundaries.
      int maxPencilRefLvl = pencils.path[pencili].size();
      int maxNbrRefLvl = 0;

      const auto* frontNeighbors = mpiGrid.get_neighbors_of(ids.front(),neighborhoodId);
      const auto* backNeighbors  = mpiGrid.get_neighbors_of(ids.back() ,neighborhoodId);


      // Create list of unique distances in the negative direction from the first cell in pencil
      std::set< int > distances;
      for (const auto nbrPair : *frontNeighbors) {
         if(nbrPair.second[dimension] < 0) {
            // gather positive values
            distances.insert(-nbrPair.second[dimension]);
         }
      }
      int foundcells = 0;
      CellID lastcell = INVALID_CELLID;
      // Iterate through distances for VLASOV_STENCIL_WIDTH elements starting from the smallest distance.
      for (auto it = distances.begin(); it != distances.end(); ++it) {
         for (const auto nbrPair : *frontNeighbors) {
            if (nbrPair.first==lastcell) continue;
            int distanceInRefinedCells = -nbrPair.second[dimension];
            if(distanceInRefinedCells == *it) {
               maxNbrRefLvl = max(maxNbrRefLvl,mpiGrid.get_refinement_level(nbrPair.first));
               lastcell = nbrPair.first;
               foundcells++;
               continue;
            }
         }
         if (foundcells >= VLASOV_STENCIL_WIDTH) break; // checked enough distances
      }

      // Create list of unique distances in the positive direction from the last cell in pencil
      distances.clear();
      for (const auto nbrPair : *backNeighbors) {
         if(nbrPair.second[dimension] > 0) {
            distances.insert(nbrPair.second[dimension]);
         }
      }
      foundcells = 0;
      lastcell = INVALID_CELLID;
      for (auto it = distances.begin(); it != distances.end(); ++it) {
         for (const auto nbrPair : *backNeighbors) {
            if (nbrPair.first==lastcell) continue;
            int distanceInRefinedCells = nbrPair.second[dimension];
            if(distanceInRefinedCells == *it) {
               maxNbrRefLvl = max(maxNbrRefLvl,mpiGrid.get_refinement_level(nbrPair.first));
               lastcell = nbrPair.first;
               foundcells++;
               continue;
            }
         }
         if (foundcells >= VLASOV_STENCIL_WIDTH) break; // checked enough distances
      }

      // Old version which can check needlessly far
      // for (const auto nbrPair: *frontNeighbors) {
      //    maxNbrRefLvl = max(maxNbrRefLvl,mpiGrid.get_refinement_level(nbrPair.first));
      // }
      // for (const auto nbrPair: *backNeighbors) {
      //    maxNbrRefLvl = max(maxNbrRefLvl,mpiGrid.get_refinement_level(nbrPair.first));
      // }


      if (maxNbrRefLvl > maxPencilRefLvl) {
         if(debug) {
            std::cout << "I am rank " << myRank << ". ";
            std::cout << "Found refinement level " << maxNbrRefLvl << " in one of the ghost cells of pencil " << pencili << ". ";
            std::cout << "Highest refinement level in this pencil is " << maxPencilRefLvl;
            std::cout << ". Splitting pencil " << pencili << endl;
         }
         // Let's avoid modifying pencils while we are looping over it. Write down the indices of pencils
         // that need to be split and split them later.
#pragma omp critical
         {
            idsToSplit.push_back(pencili);
         }
      }
   }

// No threading here, probably more efficient to thread inside the splitting
   for (auto pencili: idsToSplit) {

      Realv dx = 0.0;
      Realv dy = 0.0;
      // TODO: Double-check that this gives you the right dimensions!
      auto ids = pencils.getIds(pencili);
      switch(dimension) {
      case 0:
         dx = mpiGrid[ids[0]]->SpatialCell::parameters[CellParams::DY];
         dy = mpiGrid[ids[0]]->SpatialCell::parameters[CellParams::DZ];
         break;
      case 1:
         dx = mpiGrid[ids[0]]->SpatialCell::parameters[CellParams::DX];
         dy = mpiGrid[ids[0]]->SpatialCell::parameters[CellParams::DZ];
         break;
      case 2:
         dx = mpiGrid[ids[0]]->SpatialCell::parameters[CellParams::DX];
         dy = mpiGrid[ids[0]]->SpatialCell::parameters[CellParams::DY];
         break;
      }

// WARNING threading inside this function
      pencils.split(pencili,dx,dy);
         
   }
}

/* Checks that each local spatial cell appears in pencils at least 1 time.
 *
 * @param mpiGrid DCCRG grid object
 * @param cells Local spatial cells
 * @param pencils Pencil data struct
 */
bool checkPencils(
   const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
   const std::vector<CellID> &cells,
   const setOfPencils& pencils
) {

   bool correct = true;

   for (auto id : cells) {

      if (mpiGrid[id]->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY )  {
      
         int myCount = std::count(pencils.ids.begin(), pencils.ids.end(), id);
         
         if( myCount == 0) {
            
            std::cerr << "ERROR: Cell ID " << id << " Appears in pencils " << myCount << " times!"<< std::endl;            
            correct = false;
         }

      }
      
   }

   for (uint ipencil = 0; ipencil < pencils.N; ++ipencil) {
      cint nPencilsThroughThisCell = pow(pow(2,pencils.path[ipencil].size()),2);
      auto ids = pencils.getIds(ipencil);
      
      for (auto id : ids) {

         cint myCount = std::count(pencils.ids.begin(), pencils.ids.end(), id);

         if (myCount > nPencilsThroughThisCell) {

            std::cerr << "ERROR: Cell ID " << id << " Appears in pencils " << myCount << " times!"<< std::endl;
            std::cerr << "       It should not appear more than " << nPencilsThroughThisCell << " times." << std::endl;
            correct = false;

         }

      }

   }

   return correct;
   
}

/* Debugging function, prints the list of cells in each pencil
 *
 * @param pencils Pencil data struct
 * @param dimension Spatial dimension
 * @param myRank MPI rank
 */
void printPencilsFunc(const setOfPencils& pencils, const uint dimension, const int myRank) {
   
   // Print out ids of pencils (if needed for debugging)
   uint ibeg = 0;
   uint iend = 0;
   std::cout << "I am rank " << myRank << ", I have " << pencils.N << " pencils along dimension " << dimension << ":\n";
   MPI_Barrier(MPI_COMM_WORLD);
   if(myRank == MASTER_RANK) {
      std::cout << "t, N, mpirank, dimension (x, y): indices {path} " << std::endl;
      std::cout << "-----------------------------------------------------------------" << std::endl;
   }
   MPI_Barrier(MPI_COMM_WORLD);
   for (uint i = 0; i < pencils.N; i++) {
      iend += pencils.lengthOfPencils[i];
      std::cout << P::t << ", ";
      std::cout << i << ", ";
      std::cout << myRank << ", ";
      std::cout << dimension << ", ";
      std::cout << "(" << pencils.x[i] << ", " << pencils.y[i] << "): ";
      for (auto j = pencils.ids.begin() + ibeg; j != pencils.ids.begin() + iend; ++j) {
         std::cout << *j << " ";
      }
      ibeg  = iend;
      
      std::cout << "{";         
      for (auto step : pencils.path[i]) {
         std::cout << step << ", ";
      }
      std::cout << "}";
      
      std::cout << std::endl;
   }

   MPI_Barrier(MPI_COMM_WORLD);
}

/* Wrapper function for calling seed ID selection and pencil generation, per dimension.
 * Includes threading and gathering of pencils into thread-containers.
 *
 * @param [in] mpiGrid DCCRG grid object
 * @param [in] dimension Spatial dimension
 */
void prepareSeedIdsAndPencils(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                              const uint dimension) {

   const bool printPencils = false;
   int myRank;
   if(printPencils) MPI_Comm_rank(MPI_COMM_WORLD,&myRank);

   const vector<CellID>& localCells = getLocalCells();
   vector<CellID> localPropagatedCells;
   // Figure out which spatial cells are translated,
   // result independent of particle species.
   for (size_t c=0; c<localCells.size(); ++c) {
      if (do_translate_cell(mpiGrid[localCells[c]])) {
         localPropagatedCells.push_back(localCells[c]);
      }
   }

   phiprof::start("getSeedIds");
   vector<CellID> seedIds;
   getSeedIds(mpiGrid, localPropagatedCells, dimension, seedIds);
   phiprof::stop("getSeedIds");

   phiprof::start("buildPencils");
   // Output vectors for ready pencils
   //setOfPencils pencils;

   // Clear previous set
   DimensionPencils[dimension].removeAllPencils();
   
#pragma omp parallel
   {
      // Empty vectors for internal use of buildPencilsWithNeighbors. Could be default values but
      // default vectors are complicated. Should overload buildPencilsWithNeighbors like suggested here
      // https://stackoverflow.com/questions/3147274/c-default-argument-for-vectorint
      vector<CellID> ids;
      vector<uint> path;
      // thread-internal pencil set to be accumulated at the end
      setOfPencils thread_pencils;
      // iterators used in the accumulation
      std::vector<CellID>::iterator ibeg, iend;

#pragma omp for schedule(guided)
      for (uint i=0; i<seedIds.size(); i++) {
         cuint seedId = seedIds[i];
         // Construct pencils from the seedIds into a set of pencils.
         thread_pencils = buildPencilsWithNeighbors(mpiGrid, thread_pencils, seedId, ids, dimension, path, seedIds);
      }

      // accumulate thread results in global set of pencils
#pragma omp critical
      {
         for (uint i=0; i<thread_pencils.N; i++) {
            // Use vector range constructor
            ibeg = thread_pencils.ids.begin() + thread_pencils.idsStart[i];
            iend = ibeg + thread_pencils.lengthOfPencils[i];
            std::vector<CellID> pencilIds(ibeg, iend);
            DimensionPencils[dimension].addPencil(pencilIds,thread_pencils.x[i],thread_pencils.y[i],thread_pencils.periodic[i],thread_pencils.path[i]);
         }
      }
   }

   phiprof::start("check_ghost_cells");
   // Check refinement of two ghost cells on each end of each pencil
   check_ghost_cells(mpiGrid,DimensionPencils[dimension],dimension);
   phiprof::stop("check_ghost_cells");

   // ****************************************************************************

   if(printPencils) printPencilsFunc(DimensionPencils[dimension],dimension,myRank);
   phiprof::stop("buildPencils");
}

/* Map velocity blocks in all local cells forward by one time step in one spatial dimension.
 * This function uses 1-cell wide pencils to update cells in-place to avoid allocating large
 * temporary buffers.
 *
 * @param [in] mpiGrid DCCRG grid object
 * @param [in] localPropagatedCells List of local cells that get propagated
 * ie. not boundary or DO_NOT_COMPUTE
 * @param [in] remoteTargetCells List of non-local target cells
 * @param [in] dimension Spatial dimension
 * @param [in] dt Time step
 * @param [in] popId Particle population ID
 */
bool trans_map_1d_amr(const dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
                      const vector<CellID>& localPropagatedCells,
                      const vector<CellID>& remoteTargetCells,
                      std::vector<uint>& nPencils,
                      const uint dimension,
                      const Realv dt,
                      const uint popID) {
   
   phiprof::start("setup");

   uint cell_indices_to_id[3]; /*< used when computing id of target cell in block*/
   unsigned char  cellid_transpose[WID3]; /*< defines the transpose for the solver internal (transposed) id: i + j*WID + k*WID2 to actual one*/
   // return if there's no cells to propagate
   if(localPropagatedCells.size() == 0) {
      cout << "Returning because of no cells" << endl;
      return false;
   }

   // Vector with all cell ids
   vector<CellID> allCells(localPropagatedCells);
   allCells.insert(allCells.end(), remoteTargetCells.begin(), remoteTargetCells.end());  

   // Vectors of pointers to the cell structs
   std::vector<SpatialCell*> allCellsPointer(allCells.size());  
   
   // Initialize allCellsPointer
   #pragma omp parallel for
   for(uint celli = 0; celli < allCells.size(); celli++){
      allCellsPointer[celli] = mpiGrid[allCells[celli]];
   }

   // Fiddle indices x,y,z in VELOCITY SPACE
   switch (dimension) {
   case 0:
      // set values in array that is used to convert block indices 
      // to global ID using a dot product.
      cell_indices_to_id[0]=WID2;
      cell_indices_to_id[1]=WID;
      cell_indices_to_id[2]=1;
      break;
   case 1:
      // set values in array that is used to convert block indices 
      // to global ID using a dot product
      cell_indices_to_id[0]=1;
      cell_indices_to_id[1]=WID2;
      cell_indices_to_id[2]=WID;
      break;
   case 2:
      // set values in array that is used to convert block indices
      // to global id using a dot product.
      cell_indices_to_id[0]=1;
      cell_indices_to_id[1]=WID;
      cell_indices_to_id[2]=WID2;
      break;
   default:
      cerr << __FILE__ << ":"<< __LINE__ << " Wrong dimension, abort"<<endl;
      abort();
      break;
   }
           
   // ****************************************************************************

   // compute pencils => set of pencils (shared datastructure)
   // prepareSeedIdsAndPencils(mpiGrid,dimension); // moved to grid.cpp

   // init cellid_transpose (moved here to take advantage of the omp parallel region)
#pragma omp parallel for collapse(3)
   for (uint k=0; k<WID; ++k) {
      for (uint j=0; j<WID; ++j) {
         for (uint i=0; i<WID; ++i) {
            const uint cell =
               i * cell_indices_to_id[0] +
               j * cell_indices_to_id[1] +
               k * cell_indices_to_id[2];
            cellid_transpose[ i + j * WID + k * WID2] = cell;
         }
      }
   }

   // Warning: checkPencils fails to understand situations where pencils reach across 3 levels of refinement.
   // if(!checkPencils(mpiGrid, localPropagatedCells, pencils)) {
   //    std::cerr<<"abort checkpencils"<<std::endl;
   //    abort();
   // }
   
   if (Parameters::prepareForRebalance == true) {
      for (uint i=0; i<localPropagatedCells.size(); i++) {
         cuint myPencilCount = std::count(DimensionPencils[dimension].ids.begin(), DimensionPencils[dimension].ids.end(), localPropagatedCells[i]);
         nPencils[i] += myPencilCount;
         nPencils[nPencils.size()-1] += myPencilCount;
      }
   }
   
   // Get a pointer to the velocity mesh of the first spatial cell
   const vmesh::VelocityMesh<vmesh::GlobalID,vmesh::LocalID>& vmesh = allCellsPointer[0]->get_velocity_mesh(popID);
   
   phiprof::start("buildBlockList");
   // Get a unique sorted list of blockids that are in any of the
   // propagated cells. First use set for this, then add to vector (may not
   // be the most nice way to do this and in any case we could do it along
   // dimension for data locality reasons => copy acc map column code, TODO: FIXME
   // TODO: Do this separately for each pencil?
   std::vector<vmesh::GlobalID> unionOfBlocks;
   std::unordered_set<vmesh::GlobalID> unionOfBlocksSet;
//   unionOfBlocks.reserve(unionOfBlocksSet.size());
#pragma omp parallel
   {
      std::unordered_set<vmesh::GlobalID> thread_unionOfBlocksSet;
      
#pragma omp for
      for(unsigned int i=0; i<allCellsPointer.size(); i++) {
         auto cell = &allCellsPointer[i];
         vmesh::VelocityMesh<vmesh::GlobalID,vmesh::LocalID>& cvmesh = (*cell)->get_velocity_mesh(popID);
         for (vmesh::LocalID block_i=0; block_i< cvmesh.size(); ++block_i) {
            thread_unionOfBlocksSet.insert(cvmesh.getGlobalID(block_i));
         }
      }

#pragma omp critical
      {
         unionOfBlocksSet.insert(thread_unionOfBlocksSet.begin(), thread_unionOfBlocksSet.end());
      } // pragma omp critical
   } // pragma omp parallel
   
   unionOfBlocks.insert(unionOfBlocks.end(), unionOfBlocksSet.begin(), unionOfBlocksSet.end());
   
   phiprof::stop("buildBlockList");
   // ****************************************************************************
   
   // Assuming 1 neighbor in the target array because of the CFL condition
   // In fact propagating to > 1 neighbor will give an error
   const uint nTargetNeighborsPerPencil = 1;
   
   // Compute spatial neighbors for target cells.
   // For targets we need the local cells, plus a padding of 1 cell at both ends
   phiprof::start("computeSpatialTargetCellsForPencils");
   std::vector<SpatialCell*> targetCells(DimensionPencils[dimension].sumOfLengths + DimensionPencils[dimension].N * 2 * nTargetNeighborsPerPencil );
   computeSpatialTargetCellsForPencilsWithFaces(mpiGrid, DimensionPencils[dimension], dimension, targetCells.data());
   phiprof::stop("computeSpatialTargetCellsForPencils");
   
   
   phiprof::stop("setup");
   
   int t1 = phiprof::initializeTimer("mapping");
   int t2 = phiprof::initializeTimer("store");
   
   #pragma omp parallel
   {
      // declarations for variables needed by the threads
      std::vector<Realf, aligned_allocator<Realf, WID3>> targetBlockData((DimensionPencils[dimension].sumOfLengths + 2 * nTargetNeighborsPerPencil * DimensionPencils[dimension].N) * WID3);
      std::vector<std::vector<SpatialCell*>> pencilSourceCells;
      
      // Allocate aligned vectors which are needed once per pencil to avoid reallocating once per block loop + pencil loop iteration
      std::vector<std::vector<Vec, aligned_allocator<Vec,WID3>>> pencilTargetValues;
      std::vector<std::vector<Vec, aligned_allocator<Vec,WID3>>> pencilSourceVecData;
      std::vector<std::vector<Vec, aligned_allocator<Vec,WID3>>> pencildz;
      
      for(uint pencili = 0; pencili < DimensionPencils[dimension].N; ++pencili) {
         
         cint L = DimensionPencils[dimension].lengthOfPencils[pencili];
         cuint sourceLength = L + 2 * VLASOV_STENCIL_WIDTH;
         
         // Vector buffer where we write data, initialized to 0*/
         std::vector<Vec, aligned_allocator<Vec,WID3>> targetValues((L + 2 * nTargetNeighborsPerPencil) * WID3 / VECL);
         pencilTargetValues.push_back(targetValues);
         // Allocate source data: sourcedata<length of pencil * WID3)
         // Add padding by 2 * VLASOV_STENCIL_WIDTH
         std::vector<Vec, aligned_allocator<Vec,WID3>> sourceVecData(sourceLength * WID3 / VECL);
         pencilSourceVecData.push_back(sourceVecData);

         // Compute spatial neighbors for the source cells of the pencil. In
         // source cells we have a wider stencil and take into account boundaries.
         std::vector<SpatialCell*> sourceCells(sourceLength);
         computeSpatialSourceCellsForPencil(mpiGrid, DimensionPencils[dimension], pencili, dimension, sourceCells.data());
         pencilSourceCells.push_back(sourceCells);

         // dz is the cell size in the direction of the pencil
         std::vector<Vec, aligned_allocator<Vec,WID3>> dz(sourceLength);
         for(uint i = 0; i < sourceCells.size(); ++i) {
            dz[i] = sourceCells[i]->parameters[CellParams::DX+dimension];
         }
         pencildz.push_back(dz);
      }
      
      // Loop over velocity space blocks. Thread this loop (over vspace blocks) with OpenMP.
      #pragma omp for schedule(guided)
      for(uint blocki = 0; blocki < unionOfBlocks.size(); blocki++) {

         // Get global id of the velocity block
         vmesh::GlobalID blockGID = unionOfBlocks[blocki];

            phiprof::start(t1);
            
            // Loop over pencils
            uint totalTargetLength = 0;
            for(uint pencili = 0; pencili < DimensionPencils[dimension].N; ++pencili){
//             for ( auto pencili : unionOfBlocksMapToPencilIds.at(blockGID) ) {
               
               int L = DimensionPencils[dimension].lengthOfPencils[pencili];
               uint targetLength = L + 2 * nTargetNeighborsPerPencil;
               uint sourceLength = L + 2 * VLASOV_STENCIL_WIDTH;
                              
               // load data(=> sourcedata) / (proper xy reconstruction in future)
               bool pencil_has_data = copy_trans_block_data_amr(pencilSourceCells[pencili].data(), blockGID, L, pencilSourceVecData[pencili].data(),
                                         cellid_transpose, popID);

               if(!pencil_has_data) {
                  totalTargetLength += targetLength;
                  continue;
               }

               // Dz and sourceVecData are both padded by VLASOV_STENCIL_WIDTH
               // Dz has 1 value/cell, sourceVecData has WID3 values/cell
               propagatePencil(pencildz[pencili].data(), pencilSourceVecData[pencili].data(), pencilTargetValues[pencili].data(), dimension, blockGID, dt, vmesh, L, pencilSourceCells[pencili][0]->getVelocityBlockMinValue(popID));

               // sourceVecData => targetBlockData[this pencil])

               // Loop over cells in pencil
               for (uint icell = 0; icell < targetLength; icell++) {
                  // Loop over 1st vspace dimension
                  for (uint k=0; k<WID; k++) {
                     // Loop over 2nd vspace dimension
                     for(uint planeVector = 0; planeVector < VEC_PER_PLANE; planeVector++){

                        // Unpack the vector data
                        Realf vector[VECL];
                        //pencilSourceVecData[pencili][i_trans_ps_blockv_pencil(planeVector, k, icell - 1, L)].store(vector);
                        pencilTargetValues[pencili][i_trans_pt_blockv(planeVector, k, icell - 1)].store(vector);

                        // Loop over 3rd (vectorized) vspace dimension
                        for (uint iv = 0; iv < VECL; iv++) {

                           // Store vector data in target data array.
                           targetBlockData[(totalTargetLength + icell) * WID3 +
                                           cellid_transpose[iv + planeVector * VECL + k * WID2]]
                              = vector[iv];
                        }
                     }
                  }
               }
               totalTargetLength += targetLength;
               
            } // Closes loop over pencils. SourceVecData gets implicitly deallocated here.

            phiprof::stop(t1);
            phiprof::start(t2);
            
            // reset blocks in all non-sysboundary neighbor spatial cells for this block id
            // At this point the block data is saved in targetBlockData so we can reset the spatial cells

            for (auto *spatial_cell: targetCells) {
               // Check for null and system boundary
               if (spatial_cell && spatial_cell->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
                  
                  // Get local velocity block id
                  const vmesh::LocalID blockLID = spatial_cell->get_velocity_block_local_id(blockGID, popID);
                  
                  // Check for invalid block id
                  if (blockLID != vmesh::VelocityMesh<vmesh::GlobalID,vmesh::LocalID>::invalidLocalID()) {
                     
                     // Get a pointer to the block data
                     Realf* blockData = spatial_cell->get_data(blockLID, popID);
                     
                     // Loop over velocity block cells
                     for(int i = 0; i < WID3; i++) {
                        blockData[i] = 0.0;
                     }
                  }
               }
            }

            // store_data(target_data => targetCells)  :Aggregate data for blockid to original location 
            // Loop over pencils again
            totalTargetLength = 0;
            for(uint pencili = 0; pencili < DimensionPencils[dimension].N; pencili++){
               
               uint targetLength = DimensionPencils[dimension].lengthOfPencils[pencili] + 2 * nTargetNeighborsPerPencil;
               
               // store values from targetBlockData array to the actual blocks
               // Loop over cells in the pencil, including the padded cells of the target array
               for ( uint celli = 0; celli < targetLength; celli++ ) {
                  
                  uint GID = celli + totalTargetLength; 
                  SpatialCell* targetCell = targetCells[GID];

                  if(targetCell) { // this check also skips sysboundary cells
                  
                     const vmesh::LocalID blockLID = targetCell->get_velocity_block_local_id(blockGID, popID);

                     // Check for invalid block id
                     if( blockLID == vmesh::VelocityMesh<vmesh::GlobalID,vmesh::LocalID>::invalidLocalID() ) {
                        continue;
                     }
                     
                     Realf* blockData = targetCell->get_data(blockLID, popID);
                     
                     // areaRatio is the reatio of the cross-section of the spatial cell to the cross-section of the pencil.
                     int diff = targetCell->SpatialCell::parameters[CellParams::REFINEMENT_LEVEL] - DimensionPencils[dimension].path[pencili].size();
                     int ratio;
                     Realf areaRatio;
                     if(diff>=0) {
                        ratio = 1 << diff;
                        areaRatio = ratio*ratio;
                     } else {
                        ratio = 1 << -diff;
                        areaRatio = 1.0 / (ratio*ratio);
                     }
                     
                     for(int i = 0; i < WID3 ; i++) {
                        blockData[i] += targetBlockData[GID * WID3 + i] * areaRatio;
                     }
                  }
               }

               totalTargetLength += targetLength;
               
            } // closes loop over pencils

            phiprof::stop(t2);
      } // Closes loop over blocks
   } // closes pragma omp parallel

   return true;
}


/* Get an index that identifies which cell in the list of sibling cells this cell is.
 *
 * @param mpiGrid DCCRG grid object
 * @param cellid DCCRG id of this cell
 */
int get_sibling_index(dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid, const CellID& cellid) {

   const int NO_SIBLINGS = 0;
   const int ERROR = -1;
   
   if(mpiGrid.get_refinement_level(cellid) == 0) {
      return NO_SIBLINGS;
   }
   
   //CellID parent = mpiGrid.mapping.get_parent(cellid);
   CellID parent = mpiGrid.get_parent(cellid);

   if (parent == INVALID_CELLID) {
      std::cerr<<"Invalid parent id"<<std::endl;
      abort();
   }

   // get_all_children returns an array instead of a vector now, need to map it to a vector for find and distance
   // std::array<uint64_t, 8> siblingarr = mpiGrid.mapping.get_all_children(parent);
   // vector<CellID> siblings(siblingarr.begin(), siblingarr.end());
   vector<CellID> siblings = mpiGrid.get_all_children(parent);
   auto location = std::find(siblings.begin(),siblings.end(),cellid);
   auto index = std::distance(siblings.begin(), location);
   if (index>7) {
      std::cerr<<"Invalid parent id"<<std::endl;
      abort();
   }
   return index;
   
}

/* This function communicates the mapping on process boundaries, and then updates the data to their correct values.
 * When sending data between neighbors of different refinement levels, special care has to be taken to ensure that
 * The sending and receiving ranks allocate the correct size arrays for neighbor_block_data.
 * This is partially due to DCCRG defining neighborhood size relative to the host cell. For details, see 
 * https://github.com/fmihpc/dccrg/issues/12
 *
 * @param mpiGrid DCCRG grid object
 * @param dimension Spatial dimension
 * @param direction Direction of communication (+ or -)
 * @param popId Particle population ID
 */
void update_remote_mapping_contribution_amr(
   dccrg::Dccrg<SpatialCell,dccrg::Cartesian_Geometry>& mpiGrid,
   const uint dimension,
   int direction,
   const uint popID) {

   const vector<CellID>& local_cells = getLocalCells();
   const vector<CellID> remote_cells = mpiGrid.get_remote_cells_on_process_boundary(VLASOV_SOLVER_NEIGHBORHOOD_ID);
   vector<CellID> receive_cells;
   set<CellID> send_cells;
   
   vector<CellID> receive_origin_cells;
   vector<uint> receive_origin_index;

   int neighborhood = 0;
   
   //normalize and set neighborhoods
   if(direction > 0) {
      direction = 1;
      switch (dimension) {
      case 0:
         neighborhood = SHIFT_P_X_NEIGHBORHOOD_ID;
         break;
      case 1:
         neighborhood = SHIFT_P_Y_NEIGHBORHOOD_ID;
         break;
      case 2:
         neighborhood = SHIFT_P_Z_NEIGHBORHOOD_ID;
         break;
      }
   }
   if(direction < 0) {
      direction = -1;
      switch (dimension) {
      case 0:
         neighborhood = SHIFT_M_X_NEIGHBORHOOD_ID;
         break;
      case 1:
         neighborhood = SHIFT_M_Y_NEIGHBORHOOD_ID;
         break;
      case 2:
         neighborhood = SHIFT_M_Z_NEIGHBORHOOD_ID;
         break;
      }
   }

   // MPI_Barrier(MPI_COMM_WORLD);
   // cout << "begin update_remote_mapping_contribution_amr, dimension = " << dimension << ", direction = " << direction << endl;
   // MPI_Barrier(MPI_COMM_WORLD);

   // Initialize remote cells
   for (auto rc : remote_cells) {
      SpatialCell *ccell = mpiGrid[rc];
      // Initialize number of blocks to 0 and block data to a default value.
      // We need the default for 1 to 1 communications
      if(ccell) {
         for (uint i = 0; i < MAX_NEIGHBORS_PER_DIM; ++i) {
            ccell->neighbor_block_data[i] = ccell->get_data(popID);
            ccell->neighbor_number_of_blocks[i] = 0;
         }
      }
   }

   // Initialize local cells
   for (auto lc : local_cells) {
      SpatialCell *ccell = mpiGrid[lc];
      if(ccell) {
         // Initialize number of blocks to 0 and neighbor block data pointer to the local block data pointer
         for (uint i = 0; i < MAX_NEIGHBORS_PER_DIM; ++i) {
            ccell->neighbor_block_data[i] = ccell->get_data(popID);
            ccell->neighbor_number_of_blocks[i] = 0;
         }
      }
   }
   
   vector<Realf*> receiveBuffers;
   vector<Realf*> sendBuffers;
   
   for (auto c : local_cells) {
      
      SpatialCell *ccell = mpiGrid[c];

      if (!ccell) continue;

      const auto faceNbrs = mpiGrid.get_face_neighbors_of(c);

      vector<CellID> p_nbrs;
      vector<CellID> n_nbrs;
      
      for (const auto nbr : faceNbrs) {
         if(nbr.second == ((int)dimension + 1) * direction) {
            p_nbrs.push_back(nbr.first);
         }

         if(nbr.second == -1 * ((int)dimension + 1) * direction) {
            n_nbrs.push_back(nbr.first);
         }
      }
      
      uint sendIndex = 0;
      uint recvIndex = 0;

      int mySiblingIndex = get_sibling_index(mpiGrid,c);
      
      // Set up sends if any neighbor cells in p_nbrs are non-local.
      if (!all_of(p_nbrs.begin(), p_nbrs.end(), [&mpiGrid](CellID i){return mpiGrid.is_local(i);})) {

         // ccell adds a neighbor_block_data block for each neighbor in the positive direction to its local data
         for (const auto nbr : p_nbrs) {
            
            //Send data in nbr target array that we just mapped to, if
            // 1) it is a valid target,
            // 2) the source cell in center was translated,
            // 3) Cell is remote.
            if(nbr != INVALID_CELLID && do_translate_cell(ccell) && !mpiGrid.is_local(nbr)) {

               /*
                 Select the index to the neighbor_block_data and neighbor_number_of_blocks arrays
                 1) Ref_c == Ref_nbr == 0, index = 0
                 2) Ref_c == Ref_nbr != 0, index = c sibling index
                 3) Ref_c >  Ref_nbr     , index = c sibling index
                 4) Ref_c <  Ref_nbr     , index = nbr sibling index
                */
               
               if(mpiGrid.get_refinement_level(c) >= mpiGrid.get_refinement_level(nbr)) {
                  sendIndex = mySiblingIndex;
               } else {
                  sendIndex = get_sibling_index(mpiGrid,nbr);
               }
            
               SpatialCell *pcell = mpiGrid[nbr];
               
               // 4) it exists and is not a boundary cell,
               if(pcell && pcell->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {

                  ccell->neighbor_number_of_blocks.at(sendIndex) = pcell->get_number_of_velocity_blocks(popID);
                  
                  if(send_cells.find(nbr) == send_cells.end()) {
                     // 5 We have not already sent data from this rank to this cell.
                     
                     ccell->neighbor_block_data.at(sendIndex) = pcell->get_data(popID);
                     send_cells.insert(nbr);
                                                               
                  } else {

                     // The receiving cell can't know which cell is sending the data from this rank.
                     // Therefore, we have to send 0's from other cells in the case where multiple cells
                     // from one rank are sending to the same remote cell so that all sent cells can be
                     // summed for the correct result.
                     
                     ccell->neighbor_block_data.at(sendIndex) =
                        (Realf*) aligned_malloc(ccell->neighbor_number_of_blocks.at(sendIndex) * WID3 * sizeof(Realf), 64);
                     sendBuffers.push_back(ccell->neighbor_block_data.at(sendIndex));
                     for (uint j = 0; j < ccell->neighbor_number_of_blocks.at(sendIndex) * WID3; ++j) {
                        ccell->neighbor_block_data.at(sendIndex)[j] = 0.0;
                        
                     } // closes for(uint j = 0; j < ccell->neighbor_number_of_blocks.at(sendIndex) * WID3; ++j)
                     
                  } // closes if(send_cells.find(nbr) == send_cells.end())
                  
               } // closes if(pcell && pcell->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY)
               
            } // closes if(nbr != INVALID_CELLID && do_translate_cell(ccell) && !mpiGrid.is_local(nbr))
            
         } // closes for(uint i_nbr = 0; i_nbr < nbrs_to.size(); ++i_nbr)
        
      } // closes if(!all_of(nbrs_to.begin(), nbrs_to.end(),[&mpiGrid](CellID i){return mpiGrid.is_local(i);}))

      // Set up receives if any neighbor cells in n_nbrs are non-local.
      if (!all_of(n_nbrs.begin(), n_nbrs.end(), [&mpiGrid](CellID i){return mpiGrid.is_local(i);})) {

         // ccell adds a neighbor_block_data block for each neighbor in the positive direction to its local data
         for (const auto nbr : n_nbrs) {
         
            if (nbr != INVALID_CELLID && !mpiGrid.is_local(nbr) &&
                ccell->sysBoundaryFlag == sysboundarytype::NOT_SYSBOUNDARY) {
               //Receive data that ncell mapped to this local cell data array,
               //if 1) ncell is a valid source cell, 2) center cell is to be updated (normal cell) 3) ncell is remote

               SpatialCell *ncell = mpiGrid[nbr];

               // Check for null pointer
               if(!ncell) {
                  continue;
               }

               /*
                 Select the index to the neighbor_block_data and neighbor_number_of_blocks arrays
                 1) Ref_nbr == Ref_c == 0, index = 0
                 2) Ref_nbr == Ref_c != 0, index = nbr sibling index
                 3) Ref_nbr >  Ref_c     , index = nbr sibling index
                 4) Ref_nbr <  Ref_c     , index = c   sibling index
                */
                                             
               if(mpiGrid.get_refinement_level(nbr) >= mpiGrid.get_refinement_level(c)) {

                  // Allocate memory for one sibling at recvIndex.
                  
                  recvIndex = get_sibling_index(mpiGrid,nbr);

                  ncell->neighbor_number_of_blocks.at(recvIndex) = ccell->get_number_of_velocity_blocks(popID);
                  ncell->neighbor_block_data.at(recvIndex) =
                     (Realf*) aligned_malloc(ncell->neighbor_number_of_blocks.at(recvIndex) * WID3 * sizeof(Realf), 64);
                  receiveBuffers.push_back(ncell->neighbor_block_data.at(recvIndex));
                  
               } else {

                  recvIndex = mySiblingIndex;
                  
                  // std::array<uint64_t, 8> siblingarr = mpiGrid.mapping.get_all_children(mpiGrid.mapping.get_parent(c));
                  // vector<CellID> mySiblings(siblingarr.begin(), siblingarr.end());
                  auto mySiblings = mpiGrid.get_all_children(mpiGrid.get_parent(c));
                  auto myIndices = mpiGrid.mapping.get_indices(c);
                  
                  // Allocate memory for each sibling to receive all the data sent by coarser ncell. 
                  // only allocate blocks for face neighbors.
                  for (uint i_sib = 0; i_sib < MAX_NEIGHBORS_PER_DIM; ++i_sib) {

                     auto sibling = mySiblings.at(i_sib);
                     auto sibIndices = mpiGrid.mapping.get_indices(sibling);
                     
                     // Only allocate siblings that are remote face neighbors to ncell
                     if(mpiGrid.get_process(sibling) != mpiGrid.get_process(nbr)
                        && myIndices.at(dimension) == sibIndices.at(dimension)) {
                     
                        auto* scell = mpiGrid[sibling];
                        
                        ncell->neighbor_number_of_blocks.at(i_sib) = scell->get_number_of_velocity_blocks(popID);
                        ncell->neighbor_block_data.at(i_sib) =
                           (Realf*) aligned_malloc(ncell->neighbor_number_of_blocks.at(i_sib) * WID3 * sizeof(Realf), 64);
                        receiveBuffers.push_back(ncell->neighbor_block_data.at(i_sib));
                     }
                  }
               }
               
               receive_cells.push_back(c);
               receive_origin_cells.push_back(nbr);
               receive_origin_index.push_back(recvIndex);
               
            } // closes (nbr != INVALID_CELLID && !mpiGrid.is_local(nbr) && ...)
            
         } // closes for(uint i_nbr = 0; i_nbr < nbrs_of.size(); ++i_nbr)
         
      } // closes if(!all_of(nbrs_of.begin(), nbrs_of.end(),[&mpiGrid](CellID i){return mpiGrid.is_local(i);}))
      
   } // closes for (auto c : local_cells) {

   MPI_Barrier(MPI_COMM_WORLD);
   
   // Do communication
   SpatialCell::setCommunicatedSpecies(popID);
   SpatialCell::set_mpi_transfer_type(Transfer::NEIGHBOR_VEL_BLOCK_DATA);
   mpiGrid.update_copies_of_remote_neighbors(neighborhood);

   MPI_Barrier(MPI_COMM_WORLD);
   
   // Reduce data: sum received data in the data array to 
   // the target grid in the temporary block container   
   //#pragma omp parallel
   {
      for (size_t c = 0; c < receive_cells.size(); ++c) {
         SpatialCell* receive_cell = mpiGrid[receive_cells[c]];
         SpatialCell* origin_cell = mpiGrid[receive_origin_cells[c]];

         if(!receive_cell || !origin_cell) {
            continue;
         }
         
         Realf *blockData = receive_cell->get_data(popID);
         Realf *neighborData = origin_cell->neighbor_block_data[receive_origin_index[c]];

         //#pragma omp for 
         for(uint vCell = 0; vCell < VELOCITY_BLOCK_LENGTH * receive_cell->get_number_of_velocity_blocks(popID); ++vCell) {
            blockData[vCell] += neighborData[vCell];
         }
      }
      
      // send cell data is set to zero. This is to avoid double copy if
      // one cell is the neighbor on bot + and - side to the same process
      for (auto c : send_cells) {
         SpatialCell* spatial_cell = mpiGrid[c];
         Realf * blockData = spatial_cell->get_data(popID);
         //#pragma omp for nowait
         for(unsigned int vCell = 0; vCell < VELOCITY_BLOCK_LENGTH * spatial_cell->get_number_of_velocity_blocks(popID); ++vCell) {
            // copy received target data to temporary array where target data is stored.
            blockData[vCell] = 0;
         }
      }
   }

   for (auto p : receiveBuffers) {
      aligned_free(p);
   }
   for (auto p : sendBuffers) {
      aligned_free(p);
   }

   // MPI_Barrier(MPI_COMM_WORLD);
   // cout << "end update_remote_mapping_contribution_amr, dimension = " << dimension << ", direction = " << direction << endl;
   // MPI_Barrier(MPI_COMM_WORLD);

}
