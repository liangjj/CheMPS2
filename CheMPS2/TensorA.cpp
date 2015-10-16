/*
   CheMPS2: a spin-adapted implementation of DMRG for ab initio quantum chemistry
   Copyright (C) 2013-2015 Sebastian Wouters

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

#include "TensorA.h"
#include "Lapack.h"

CheMPS2::TensorA::TensorA(const int indexIn, const int IdiffIn, const bool movingRightIn, const SyBookkeeper * denBKIn) :
TensorOperator(indexIn,
               0, // two_j
               2, // n_elec
               IdiffIn,
               movingRightIn,
               true,  // prime_last (doesn't matter for spin-0)
               false, // jw_phase (two 2nd quantized operators)
               denBKIn){ }

CheMPS2::TensorA::~TensorA(){ }

void CheMPS2::TensorA::ClearStorage(){ clear(); }

void CheMPS2::TensorA::AddATerm(double alpha, TensorOperator * TermToAdd){

   int inc = 1;
   daxpy_(kappa2index+nKappa, &alpha, TermToAdd->gStorage(), &inc, storage, &inc);

}

