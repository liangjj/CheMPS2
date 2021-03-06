/*
   CheMPS2: a spin-adapted implementation of DMRG for ab initio quantum chemistry
   Copyright (C) 2013 Sebastian Wouters

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <stdlib.h>
#include <iostream>
#include <string>
#include <math.h>
#include <algorithm>
#include <sys/stat.h>

#include "CASSCF.h"
#include "Lapack.h"

using std::string;
using std::ifstream;
using std::cout;
using std::endl;
using std::max;

double CheMPS2::CASSCF::doCASSCFnewtonraphson(const int Nelectrons, const int TwoS, const int Irrep, ConvergenceScheme * OptScheme, const int rootNum){

   double gradNorm = 1.0;
   double Energy;
   
   Hamiltonian * HamDMRG = new Hamiltonian(nOrbDMRG, SymmInfo.getGroupNumber(), irrepOfEachDMRGOrbital);
   const int N = Nelectrons - 2*nCondensed;
   Problem * Prob = new Problem(HamDMRG, TwoS, N, Irrep);
   Prob->SetupReorderD2h(); //Doesn't matter if the group isn't d2h, Prob checks it.
   
   int maxlinsize = 0;
   for (int cnt=0; cnt<numberOfIrreps; cnt++){ if (OrbPerIrrep[cnt] > maxlinsize){ maxlinsize = OrbPerIrrep[cnt]; } }
   const bool doBlockWise = (maxlinsize <= CheMPS2::CASSCF_maxlinsizeCutoff) ? false : true; //Only if bigger, do we want to work blockwise
   int maxBlockSize = maxlinsize;
   if (doBlockWise){
      int factor   = (int) (ceil( (1.0 * maxlinsize) / CheMPS2::CASSCF_maxlinsizeCutoff ) + 0.01);
      maxBlockSize = (int) (ceil( (1.0 * maxlinsize) / factor ) + 0.01);
      cout << "CASSCF info: the max. # orb per irrep = " << maxlinsize << " and was truncated to " << maxBlockSize << " for the 2-body rotation." << endl;
   }
   
   //One array is approx (maxBlockSize/273.0)^4 * 42 GiB --> [maxBlockSize=100 --> 750 MB]
   int maxBSpower4 = maxBlockSize * maxBlockSize * maxBlockSize * maxBlockSize; //Note that 273**4 overfloats the 32 bit integer!!!!!
   double * mem1 = new double[max( maxBSpower4 , maxlinsize*maxlinsize*3 )]; //Second argument for updateUnitary
   double * mem2 = new double[max( maxBSpower4 , maxlinsize*maxlinsize*2 )];
   double * mem3 = NULL;
   if (doBlockWise){ mem3 = new double[maxBSpower4]; }
   
   if (CheMPS2::CASSCF_storeUnitary){
   
      struct stat stFileInfo;
      int intStat = stat(CheMPS2::CASSCF_unitaryStorageName.c_str(),&stFileInfo);
      if (intStat==0){ loadU(); }
   
   }

   while (gradNorm > CheMPS2::CASSCF_gradientNormThreshold){
   
      //Update the unitary transformations based on the previous unitary transformation and the xmatrix
      updateUnitary(mem1, mem2);
      
      if ((CheMPS2::CASSCF_storeUnitary) && (gradNorm!=1.0)){ saveU(); }
   
      //Setup rotated Hamiltonian matrix elements based on unitary transformations
      if (doBlockWise){ fillRotatedHamInMemoryBlockWise(mem1, mem2, mem3, maxBlockSize); }
      else{             fillRotatedHamAllInMemory(mem1, mem2); }
   
      //Fill HamDMRG based on the HamRotated
      fillHamDMRG(HamDMRG);
      
      //Do the DMRG sweeps, and calculate the 2DM
      DMRG * theDMRG = new DMRG(Prob,OptScheme);
      Energy = theDMRG->Solve();
      if (rootNum>1){
         theDMRG->activateExcitations(rootNum-1);
         for (int exc=0; exc<rootNum-1; exc++){
            theDMRG->newExcitation(fabs(Energy));
            Energy = theDMRG->Solve();
         }
      }
      theDMRG->calc2DM();
      theDMRG2DM = theDMRG->get2DM();
      //theDMRG2DM->print2DMAandB_HAM();
      setDMRG1DM(N);
      calcNOON();
      
      if (CheMPS2::CASSCF_debugPrint){ check1DMand2DMrotated(Nelectrons); }
      
      buildFmat(); //Needs to be updated before the Fmat and Wmat functions
      //gradNorm = updateXmatrixNewtonRaphson();
      gradNorm = updateXmatrixAugmentedHessianNR();
      
      //PrintCoeff_C2(theDMRG); //Print coeff for C2 to discern (for 1Ag) ^1Sigma_g^+ <--> ^1Delta_g   &   (for 3B1u) ^3Sigma_u^+ <--> ^3Delta_u
      
      if (CheMPS2::DMRG_storeMpsOnDisk){        theDMRG->deleteStoredMPS();       }
      if (CheMPS2::DMRG_storeRenormOptrOnDisk){ theDMRG->deleteStoredOperators(); }
      delete theDMRG;
   
   }
   
   delete [] mem1;
   delete [] mem2;
   if (doBlockWise){ delete [] mem3; }
   
   delete Prob;
   delete HamDMRG;
   
   return Energy;

}

double CheMPS2::CASSCF::updateXmatrixAugmentedHessianNR(){
   
   //Calculate the gradient
   double * gradient = new double[x_linearlength];
   double gradNorm = calcGradient(gradient);
   
   //Calculate the Hessian
   int aug_linlength = x_linearlength+1;
   int size = aug_linlength * aug_linlength;
   double * hessian = new double[size];
   calcHessian(hessian, x_linearlength+1);
   
   //Augment the gradient into the Hessian matrix
   for (int cnt=0; cnt<x_linearlength; cnt++){
      hessian[cnt + x_linearlength*aug_linlength] = gradient[cnt];
      hessian[x_linearlength + aug_linlength*cnt] = gradient[cnt];
   }
   hessian[x_linearlength + aug_linlength*x_linearlength] = 0.0;
   
   //Find its lowest eigenvalue and vector
   double * work = new double[size];
   double * eigen = new double[aug_linlength];
   char jobz = 'V';
   char uplo = 'U';
   int info;
   dsyev_(&jobz,&uplo,&aug_linlength,hessian,&aug_linlength,eigen,work,&size,&info);
   
   if (CheMPS2::CASSCF_debugPrint){
      cout << "Lowest eigenvalue = " << eigen[0] << endl;
      cout << "The last number of the eigenvector (which will be rescaled to one) = " << hessian[x_linearlength] << endl;
   }
   double scalar = 1.0/hessian[x_linearlength];
   int inc = 1;
   dscal_(&x_linearlength,&scalar,hessian,&inc);
   
   //Copy the new x back to xmatrix.
   copyXsolutionBack(hessian);
   
   delete [] hessian;
   delete [] gradient;
   delete [] eigen;
   delete [] work;
   
   return gradNorm;

}

double CheMPS2::CASSCF::updateXmatrixNewtonRaphson(){
   
   //Calculate the gradient
   double * gradient = new double[x_linearlength];
   double gradNorm = calcGradient(gradient);
   
   //Calculate the Hessian
   int size = x_linearlength * x_linearlength;
   double * hessian = new double[size];
   calcHessian(hessian, x_linearlength);
   
   //Invert the Hessian
   double * inverse = new double[size];
   double * vector = new double[x_linearlength];
   char jobz = 'V';
   char uplo = 'U';
   int info;
   dsyev_(&jobz,&uplo,&x_linearlength,hessian,&x_linearlength,vector,inverse,&size,&info);
   
   if (vector[0]<=0.0){
      cout << "CASSCF :: Eigenvalues of the Hessian = [ ";
      for (int cnt=0; cnt<x_linearlength-1; cnt++){ cout << vector[cnt] << " , "; }
      cout << vector[x_linearlength-1] << " ]" << endl;
   }
   
   for (int cnt=0; cnt<x_linearlength; cnt++){
      double value = 1.0/sqrt(vector[cnt]);
      for (int cnt2=0; cnt2<x_linearlength; cnt2++){
         hessian[cnt2 + x_linearlength*cnt] *= value;
      }
   }
   char notr = 'N';
   char tran = 'T';
   double afac = 1.0;
   double bfac = 0.0; //set
   dgemm_(&notr,&tran,&x_linearlength,&x_linearlength,&x_linearlength,&afac,hessian,&x_linearlength,hessian,&x_linearlength,&bfac,inverse,&x_linearlength);
   
   //Calculate the new x --> Eq. (6c) of the Siegbahn paper
   afac = -1.0;
   int one = 1;
   dgemm_(&notr,&notr,&x_linearlength,&one,&x_linearlength,&afac,inverse,&x_linearlength,gradient,&x_linearlength,&bfac,vector,&x_linearlength);
   
   //Copy the new x back to xmatrix.
   copyXsolutionBack(vector);
   
   delete [] hessian;
   delete [] gradient;
   delete [] vector;
   delete [] inverse;
   
   return gradNorm;

}

double CheMPS2::CASSCF::calcGradient(double * gradient){

   #pragma omp parallel for schedule(static)
   for (int cnt=0; cnt<x_linearlength; cnt++){
      gradient[cnt] = 2 * ( Fmat(x_firstindex[cnt],x_secondindex[cnt]) - Fmat(x_secondindex[cnt],x_firstindex[cnt]) );
   }
   
   double gradNorm = 0.0;
   for (int cnt=0; cnt<x_linearlength; cnt++){ gradNorm += gradient[cnt] * gradient[cnt]; }
   gradNorm = sqrt(gradNorm);
   cout << "CASSCF :: Norm of the gradient = " << gradNorm << endl;
   return gradNorm;

}

void CheMPS2::CASSCF::calcHessian(double * hessian, const int rowjump){

   const int lindim = (x_linearlength * (x_linearlength + 1))/2;
   
   #pragma omp parallel for schedule(static)
   for (int count=0; count<lindim; count++){
   
      int col = 1;
      while ( (col*(col+1))/2 <= count ) col++;
      col -= 1;
      int row = count - (col*(col+1))/2;
      
      int p_index = x_firstindex[row];
      int q_index = x_secondindex[row];
      int r_index = x_firstindex[col];
      int s_index = x_secondindex[col];
      hessian[row + rowjump * col] = Wmat(p_index,q_index,r_index,s_index)
                                   - Wmat(q_index,p_index,r_index,s_index)
                                   - Wmat(p_index,q_index,s_index,r_index)
                                   + Wmat(q_index,p_index,s_index,r_index);
      hessian[col + rowjump * row] = hessian[row + rowjump * col];
      
   }

}

double CheMPS2::CASSCF::Wmat(const int index1, const int index2, const int index3, const int index4) const{

   int irrep1 = 0;
   while (index1 >= jumpsHamOrig[irrep1+1]){ irrep1++; }
   int irrep2 = 0;
   while (index2 >= jumpsHamOrig[irrep2+1]){ irrep2++; }
   
   if (irrep1 != irrep2){ return 0.0; } //From now on: irrep1 == irrep2
   
   int irrep3 = 0;
   while (index3 >= jumpsHamOrig[irrep3+1]){ irrep3++; }
   int irrep4 = 0;
   while (index4 >= jumpsHamOrig[irrep4+1]){ irrep4++; }
   
   if (irrep3 != irrep4){ return 0.0; } //From now on: irrep3 == irrep4
   
   double value = 0.0;
   
   if (irrep1 == irrep3){ //irrep1 == irrep2 == irrep3 == irrep4
      value += 2 * get1DMrotated(index1,index3) * HamRotated->getTmat(index2,index4);
      if (index2 == index3){ value += Fmat(index1,index4) + Fmat(index4,index1); }
   }
   
   if (index1 > jumpsHamOrig[irrep1] + Nocc[irrep1] + NDMRG[irrep1]){ return value; }
   if (index3 > jumpsHamOrig[irrep3] + Nocc[irrep3] + NDMRG[irrep3]){ return value; } //index1 and index3 are now certainly not virtual!
   
   if (index1 < jumpsHamOrig[irrep1] + Nocc[irrep1]){
      if (index3 < jumpsHamOrig[irrep3] + Nocc[irrep3]){
      
         // (index1,index3) (occupied,occupied) --> (alpha,beta) can be (occupied,occupied) or (active,active)
         if (index1==index3){
         
            // Case1: (alpha,beta) is (active,active) --> index1 == index3 needed to return a non-zero element
            for (int irrep_ab=0; irrep_ab<numberOfIrreps; irrep_ab++){
               for (int alpha_index=jumpsHamOrig[irrep_ab]+Nocc[irrep_ab]; alpha_index<jumpsHamOrig[irrep_ab+1]-Nvirt[irrep_ab]; alpha_index++){ //alpha act
                  for (int beta_index=jumpsHamOrig[irrep_ab]+Nocc[irrep_ab]; beta_index<jumpsHamOrig[irrep_ab+1]-Nvirt[irrep_ab]; beta_index++){ //beta act
                     value += 2 * get1DMrotated(alpha_index,beta_index) * ( 2 * HamRotated->getVmat(index2,alpha_index,index4,beta_index)
                                                                              - HamRotated->getVmat(index2,index4,alpha_index,beta_index) );
                  }
               }
            }
            
            // Case2: (alpha,beta) is (occupied,occupied)
            // Case2a: (index1==index3) and (alpha==beta)
            for (int irrep_ab=0; irrep_ab<numberOfIrreps; irrep_ab++){
               for (int alpha_index=jumpsHamOrig[irrep_ab]; alpha_index<jumpsHamOrig[irrep_ab]+Nocc[irrep_ab]; alpha_index++){
                  if (index1 == alpha_index){
                     value += 4 * HamRotated->getVmat(index2,alpha_index,index4,alpha_index) + 4 * HamRotated->getVmat(index2,index4,alpha_index,alpha_index);
                  } else {
                     value += 8 * HamRotated->getVmat(index2,alpha_index,index4,alpha_index) - 4 * HamRotated->getVmat(index2,index4,alpha_index,alpha_index);
                  }
               }
            }
            
            // Case2b: (index1==index3); some element :-)
            value += 4 * HamRotated->getVmat(index2,index4,index1,index3);
            
         } else { //index1 != index3
         
            // Case2b: (index1!=index3); remaining elements :-)
            value += 16 * HamRotated->getVmat(index2,index4,index1,index3)
                   - 4  * HamRotated->getVmat(index2,index1,index4,index3)
                   - 4  * HamRotated->getVmat(index2,index4,index3,index1);
         
         }
      
      } else {
      
         // (index1,index3) (occupied,active) --> (alpha,beta) can be (active,occupied) or (occupied,active)
         for (int alpha_index=jumpsHamOrig[irrep3]+Nocc[irrep3]; alpha_index<jumpsHamOrig[irrep3+1]-Nvirt[irrep3]; alpha_index++){
            value += 2 * get1DMrotated(index3,alpha_index) * ( 4 * HamRotated->getVmat(index2,index4,index1,alpha_index) 
                                                                 - HamRotated->getVmat(index2,index4,alpha_index,index1) 
                                                                 - HamRotated->getVmat(index2,index1,index4,alpha_index) );
         }
         
      }
   } else {
      if (index3 < jumpsHamOrig[irrep3] + Nocc[irrep3]){
      
         // (index1,index3) (active,occupied) --> (alpha,beta) can be (active,occupied) or (occupied,active)
         for (int alpha_index=jumpsHamOrig[irrep1]+Nocc[irrep1]; alpha_index<jumpsHamOrig[irrep1+1]-Nvirt[irrep1]; alpha_index++){
            value += 2 * get1DMrotated(index1,alpha_index) * ( 4 * HamRotated->getVmat(index2,index4,alpha_index,index3) 
                                                                 - HamRotated->getVmat(index2,alpha_index,index4,index3) 
                                                                 - HamRotated->getVmat(index2,index4,index3,alpha_index) );
         }
         
      } else {
      
         // (index1,index3) (active,active) --> (alpha,beta) can be (occupied,occupied) or (active,active)
         // Case1: (alpha,beta)==(occ,occ) --> alpha == beta
         double sum = 0.0;
         for (int irrep_alpha=0; irrep_alpha<numberOfIrreps; irrep_alpha++){
            for (int alpha_index=jumpsHamOrig[irrep_alpha]; alpha_index<jumpsHamOrig[irrep_alpha]+Nocc[irrep_alpha]; alpha_index++){
               sum += 2 * HamRotated->getVmat(index2,alpha_index,index4,alpha_index) - HamRotated->getVmat(index2,index4,alpha_index,alpha_index);
            }
         }
         value += sum * 2 * get1DMrotated(index1,index3);
         
         // Case2: (alpha,beta)==(act,act)
         const int productIrrep = SymmInfo.directProd(irrep1,irrep3);
         for (int irrep_alpha=0; irrep_alpha<numberOfIrreps; irrep_alpha++){
            int irrep_beta = SymmInfo.directProd(productIrrep,irrep_alpha);
            for (int alpha_index=jumpsHamOrig[irrep_alpha]+Nocc[irrep_alpha]; alpha_index<jumpsHamOrig[irrep_alpha+1]-Nvirt[irrep_alpha]; alpha_index++){
               for (int beta_index=jumpsHamOrig[irrep_beta]+Nocc[irrep_beta]; beta_index<jumpsHamOrig[irrep_beta+1]-Nvirt[irrep_beta]; beta_index++){
                  value += 2 * (   get2DMrotated(index3,alpha_index,index1,beta_index) * HamRotated->getVmat(index2,alpha_index,index4,beta_index)
                               + ( get2DMrotated(index3,alpha_index,beta_index,index1) + get2DMrotated(index3,index1,beta_index,alpha_index) ) 
                               * HamRotated->getVmat(index2,index4,alpha_index,beta_index) );
               }
            }
         }
         
      }
   }
   
   return value;

}

void CheMPS2::CASSCF::buildFmat(){

   for (int cnt=0; cnt<numberOfIrreps; cnt++){
      const int nOrbitals = OrbPerIrrep[cnt];
      for (int cnt2=0; cnt2<nOrbitals*nOrbitals; cnt2++){
         int row = cnt2 % nOrbitals;
         int col = cnt2 / nOrbitals;
         Fmatrix[cnt][cnt2] = FmatHelper(jumpsHamOrig[cnt] + row, jumpsHamOrig[cnt] + col);
      }
   }

}

double CheMPS2::CASSCF::Fmat(const int index1, const int index2) const{

   int irrep1 = 0;
   while (index1 >= jumpsHamOrig[irrep1+1]){ irrep1++; }
   int irrep2 = 0;
   while (index2 >= jumpsHamOrig[irrep2+1]){ irrep2++; }
   
   if (irrep1 != irrep2){ return 0.0; } //From now on: both irreps are the same.
   
   return Fmatrix[irrep1][ index1 - jumpsHamOrig[irrep1] + OrbPerIrrep[irrep1] * ( index2 - jumpsHamOrig[irrep2] ) ];

}

double CheMPS2::CASSCF::FmatHelper(const int index1, const int index2) const{
   
   int irrep1 = 0;
   while (index1 >= jumpsHamOrig[irrep1+1]){ irrep1++; }
   int irrep2 = 0;
   while (index2 >= jumpsHamOrig[irrep2+1]){ irrep2++; }
   
   if (irrep1 != irrep2){ return 0.0; } //From now on: both irreps are the same.
   
   if (index1 >= jumpsHamOrig[irrep1] + Nocc[irrep1] + NDMRG[irrep1]){ return 0.0; } //index1 virtual: 1/2DM returns 0.0
   
   double value = 0.0;
   
   if (index1 < jumpsHamOrig[irrep1] + Nocc[irrep1]){ //index1 occupied
   
      //1DM will only return non-zero if r_index==index1, and then returns 2.0
      value += 2 * HamRotated->getTmat(index2,index1);
      
      //irrep1 is the irrep of index1 and index2
      for (int irrep_sum=0; irrep_sum<numberOfIrreps; irrep_sum++){ //irrep_sum is the irrep of r_index and s_index
      
         for (int r_index=jumpsHamOrig[irrep_sum]; r_index<jumpsHamOrig[irrep_sum]+Nocc[irrep_sum]; r_index++){ //r_index occupied --> delta(r_index,s_index)
            if (r_index != index1){
               value += 4 * HamRotated->getVmat(index2, r_index, index1, r_index) - 2 * HamRotated->getVmat(index2, r_index, r_index, index1);
            } else { //r_index equal to index1
               value += 2 * HamRotated->getVmat(index2, index1, index1, index1);
            }
         }
      
         for (int r_index=jumpsHamOrig[irrep_sum]+Nocc[irrep_sum]; r_index<jumpsHamOrig[irrep_sum+1]-Nvirt[irrep_sum]; r_index++){ //r_index act --> s_index act
            for (int s_index=jumpsHamOrig[irrep_sum]+Nocc[irrep_sum]; s_index<jumpsHamOrig[irrep_sum+1]-Nvirt[irrep_sum]; s_index++){ //s_index act, index1 occ
               value += get1DMrotated(r_index, s_index)
                        * (2*HamRotated->getVmat(index2, r_index, index1, s_index) - HamRotated->getVmat(index2, r_index, s_index, index1));
            }
         }
         
         //r_index virtual --> 2DM return 0.0
         
      }
   
   } else { //index1 active
   
      //1DM will only return non-zero if r_index also active, and corresponds to the same irrep
      for (int r_index=jumpsHamOrig[irrep1]+Nocc[irrep1]; r_index<jumpsHamOrig[irrep1+1]-Nvirt[irrep1]; r_index++){
         value += get1DMrotated(index1,r_index) * HamRotated->getTmat(index2,r_index);
      }
      
      //Two of the summation indices are occupied (and equal) --> the two active indices have the same irrep
      for (int s_index=jumpsHamOrig[irrep1]+Nocc[irrep1]; s_index<jumpsHamOrig[irrep1+1]-Nvirt[irrep1]; s_index++){
         const double OneDMelement = get1DMrotated(index1, s_index);
         for (int irrep_r=0; irrep_r<numberOfIrreps; irrep_r++){
            for (int r_index=jumpsHamOrig[irrep_r]; r_index<jumpsHamOrig[irrep_r]+Nocc[irrep_r]; r_index++){
               value += OneDMelement * ( 2 * HamRotated->getVmat(index2, r_index, s_index, r_index) - HamRotated->getVmat(index2, r_index, r_index, s_index) );
            }
         }
      }
      
      //All the summation indices are active
      for (int irrep_r=0; irrep_r<numberOfIrreps; irrep_r++){
         const int productIrrep = SymmInfo.directProd(irrep1,irrep_r);
         for (int irrep_s=0; irrep_s<numberOfIrreps; irrep_s++){
            int irrep_t = SymmInfo.directProd(productIrrep,irrep_s);
            for (int r_index=jumpsHamOrig[irrep_r]+Nocc[irrep_r]; r_index<jumpsHamOrig[irrep_r+1]-Nvirt[irrep_r]; r_index++){
               for (int s_index=jumpsHamOrig[irrep_s]+Nocc[irrep_s]; s_index<jumpsHamOrig[irrep_s+1]-Nvirt[irrep_s]; s_index++){
                  for (int t_index=jumpsHamOrig[irrep_t]+Nocc[irrep_t]; t_index<jumpsHamOrig[irrep_t+1]-Nvirt[irrep_t]; t_index++){
                     value += get2DMrotated(index1, r_index, s_index, t_index) * HamRotated->getVmat(index2, r_index, s_index, t_index);
                  }
               }
            }
         }
      }
   
   }
   
   return value;

}


