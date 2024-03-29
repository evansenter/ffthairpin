#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include "delta.h"
#include "misc.h"
#include <fftw3.h>
#include "energy_const.h"
#include "energy_par.h"
#include <iostream>
#include <limits>

#define STRUCTURE_COUNT 1
#define MIN_PAIR_DIST 3
#define MAX_INTERIOR_DIST 30
#define ZERO_C dcomplex(0.0, 0.0)
#define ONE_C dcomplex(1.0, 0.0)
#define FFTW_REAL 0
#define FFTW_IMAG 1
#define WINDOW_SIZE(i) (MIN_WINDOW_SIZE + i)
#define NUM_WINDOWS (WINDOW_SIZE - MIN_WINDOW_SIZE + 1)
#define ROOT_POW(i, pow, n) (rootsOfUnity[(i * pow) % (n + 1)])
#define FFTBOR_DEBUG 0
#define ENERGY_DEBUG (0 && !root)

extern int    NS, PF, N, PRECISION, WINDOW_SIZE, MIN_WINDOW_SIZE;
extern double temperature;
extern char   *ENERGY;
extern paramT *P;

extern "C" void read_parameter_file(const char energyfile[]);

void neighbours(char *inputSequence) {
  int i, root, runLength = 0, sequenceLength = strlen(inputSequence);
  double RT = 0.0019872370936902486 * (temperature + 273.15) * 100; // 0.01 * (kcal K) / mol

  char *energyfile  = ENERGY;
  char *sequence    = new char[sequenceLength + 1];
  int  *intSequence = (int *)xcalloc(sequenceLength + 1, sizeof(int));
  int **canBasePair;
  //int **numBasePairs;
  
  // Load energy parameters.
  read_parameter_file(energyfile);
  P = scale_parameters();

  // Make necessary versions of the sequence for efficient processing.
	sequence[0] = '@';
  strncpy(sequence + 1, inputSequence, sequenceLength);
  translateToIntSequence(inputSequence, intSequence);
  
  // Initialize canBasePair lookup for all 4x4 nt. combinations.
  canBasePair = (int **)xcalloc(5, sizeof(int *));
  for (i = 0; i < 5; ++i) {
    canBasePair[i] = (int *)xcalloc(5, sizeof(int));
  }
  initializeCanBasePair(canBasePair);
  
  /* Initialize numBasePairs lookup for all (i, j) combinations.
  numBasePairs = (int **)xcalloc(sequenceLength + 1, sizeof(int *));
  for (i = 1; i <= sequenceLength; i++) {
    numBasePairs[i] = (int *)xcalloc(sequenceLength + 1, sizeof(int));
  }
  initializeBasePairCounts(numBasePairs, bpList, sequenceLength);
  */
  /* Determine max bp. distance to determine how many roots of unity to generate.
  for (i = 1; i <= sequenceLength; ++i) {
    runLength += (bpList[i] > i ? 1 : 0);
  }*/
  //runLength = floor((sequenceLength - MIN_PAIR_DIST) / 2);
  runLength = floor((sequenceLength) / 7);
  dcomplex **Z            = new dcomplex*[sequenceLength + 1];
  dcomplex **ZB           = new dcomplex*[sequenceLength + 1];
  dcomplex **ZM           = new dcomplex*[sequenceLength + 1];
  dcomplex **ZM1          = new dcomplex*[sequenceLength + 1];
  dcomplex ***solutions   = new dcomplex**[NUM_WINDOWS];
  dcomplex *rootsOfUnity  = new dcomplex[runLength + 1];
  
  populateMatrices(Z, ZB, ZM, ZM1, solutions, rootsOfUnity, sequenceLength, runLength);
  
  // Start main recursions (root <= ceil(runLength / 2.0) is an optimization for roots of unity).
  if(NS==0){
    for (root = 0; root <= ceil(runLength / 2.0); ++root) {
      evaluateZ(root, Z, ZB, ZM, ZM1, solutions, rootsOfUnity, inputSequence, sequence, intSequence, canBasePair, sequenceLength, runLength, RT);
    }
  } else {
      for (root = 0; root <= ceil(runLength / 2.0); ++root) {
        evaluateZNS(root, Z, ZB, ZM, ZM1, solutions, rootsOfUnity, inputSequence, sequence, intSequence, canBasePair, sequenceLength, runLength, RT);
    }
  }
	if (FFTBOR_DEBUG) {
		std::cout << std::endl;
    printf("Number of structures: %.0f\n", Z[sequenceLength][1].real());
	}

  // Convert point-value solutions to coefficient form w/ inverse DFT.
  populateRemainingRoots(solutions, sequenceLength, runLength, root);
  solveSystem(solutions, sequence,  sequenceLength, runLength);
  
  free(intSequence);
}

void evaluateZ(int root, dcomplex **Z, dcomplex **ZB, dcomplex **ZM, dcomplex **ZM1, dcomplex ***solutions, dcomplex *rootsOfUnity, char *inputSequence, char *sequence, int *intSequence, int *canBasePair[5], int sequenceLength, int runLength, double RT) {
  int i, j, k, l, r, d, delta;
  double energy;
  
  flushMatrices(Z, ZB, ZM, ZM1, sequenceLength,rootsOfUnity[root]);
    
  if (ENERGY_DEBUG) {
    printf("RT: %f\n", RT / 100);
  }
  
  for (d = MIN_PAIR_DIST + 1; d < sequenceLength; ++d) {
    for (i = 1; i <= sequenceLength - d; ++i) {
      j = i + d;
      
      if (canBasePair[intSequence[i]][intSequence[j]]) {
        // ****************************************************************************
        // Solve ZB 
        // ****************************************************************************
        // In a hairpin, [i + 1, j - 1] unpaired.
        energy    = hairpinloop(i, j, canBasePair[intSequence[i]][intSequence[j]], intSequence, inputSequence);
        //delta     = numBasePairs[i][j] + jPairedTo(i, j, bpList);
        delta = 0;
        ZB[i][j] += ROOT_POW(root, 1, runLength) *  exp(-energy / RT);

        if (ENERGY_DEBUG) {
          printf("%+f: GetHairpinEnergy(%c (%d), %c (%d));\n", energy / 100, sequence[i], i, sequence[j], j);
        }

        if (STRUCTURE_COUNT) {
          ZB[j][i] += 1;
        }

        // Interior loop / bulge / stack / multiloop.
        for (l = i + 1; l < min2(i+30,j- MIN_PAIR_DIST-2)+1; ++l) {
          for (r = max2(l + MIN_PAIR_DIST + 1, j - (30 - (l - i))); r < j; ++r) {
            if (canBasePair[intSequence[l]][intSequence[r]]) {
               // In interior loop / bulge / stack with (i, j) and (k, l), (i + 1, k - 1) and (l + 1, j - 1) are all unpaired.
               energy    = interiorloop(i, j, l, r, canBasePair[intSequence[i]][intSequence[j]], canBasePair[intSequence[r]][intSequence[l]], intSequence);
               //delta     = numBasePairs[i][j] - numBasePairs[k][l] + jPairedTo(i, j, bpList);
               ZB[i][j] += ZB[l][r] /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);
                 
               if (ENERGY_DEBUG) {
                 printf("%+f: GetInteriorStackingAndBulgeEnergy(%c (%d), %c (%d), %c (%d), %c (%d));\n", energy / 100, sequence[i], i, sequence[j], j, sequence[k], k, sequence[l], l);
               }

               if (STRUCTURE_COUNT) {
                 ZB[j][i] += ZB[r][l];
               }
            }
         }
       }
       for(k=i+MIN_PAIR_DIST+3;k<j-4;++k){ 
          // If (i, j) is the closing b.p. of a multiloop, and (k, l) is the rightmost base pair, there is at least one hairpin between (i + 1, k - 1).
          energy    = multiloop_closing(i, j, k, l, canBasePair[intSequence[j]][intSequence[i]], canBasePair[intSequence[k]][intSequence[l]], intSequence);
          //delta     = numBasePairs[i][j] - numBasePairs[i + 1][k - 1] - numBasePairs[k][l] + jPairedTo(i, j, bpList);
          ZB[i][j] += ZM[i + 1][k - 1] * ZM1[k][j-1] /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);
                  
          if (ENERGY_DEBUG) {
            printf("%+f: MultiloopA + 2 * MultiloopB + MultiloopC * (%d - %d - 1);\n", energy / 100, j, l);
          }

          if (STRUCTURE_COUNT) {
            ZB[j][i] += ZM[k - 1][i + 1] * ZM1[j-1][k];
          }  
       }        
      }

      // ****************************************************************************
      // Solve ZM1
      // ****************************************************************************
      for(k=i+MIN_PAIR_DIST+1;k<j+1;++k){
       if(canBasePair[intSequence[i]][intSequence[k]]){
        energy = P->MLbase*(j-k);
        //IVAN
        energy += P->MLintern[canBasePair[intSequence[i]][intSequence[k]]];
        ZM1[i][j] += ZB[i][k]* exp(-energy/RT);
        if (STRUCTURE_COUNT) {
          ZM1[j][i] += ZB[k][i];
        }
       }
      }
        
      // ****************************************************************************
      // Solve ZM
      // ****************************************************************************
      //energy    = P->MLbase;
      //delta     = jPairedIn(i, j, bpList);
      //ZM[i][j] += ZM[i][j - 1] * /*ROOT_POW(root, delta, runLength) */ exp(-energy / RT);

      /*if (ENERGY_DEBUG) {
        printf("%+f: MultiloopC;\n", energy / 100);
      }

      if (STRUCTURE_COUNT) {
        ZM[j][i] += ZM[j - 1][i];
      }*/

      for (k = i; k < j - MIN_PAIR_DIST; ++k) {
        //if (canBasePair[intSequence[k]][intSequence[j]]) {
          // Only one stem.
          energy    = /*P->MLintern[canBasePair[intSequence[k]][intSequence[j]]] +*/ P->MLbase * (k - i);
          //delta     = numBasePairs[i][j] - numBasePairs[k][j];
          ZM[i][j] += ZM1[k][j] /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

          if (ENERGY_DEBUG) {
            printf("%+f: MultiloopB + MultiloopC * (%d - %d);\n", energy / 100, k, i);
          }

          if (STRUCTURE_COUNT) {
            ZM[j][i] += ZM1[j][k];
          }

          // More than one stem.
          if (k >= i + MIN_PAIR_DIST + 2) {
            //energy    = P->MLintern[canBasePair[intSequence[k]][intSequence[j]]];
            //delta     = numBasePairs[i][j] - numBasePairs[i][k - 1] - numBasePairs[k][j];
            ZM[i][j] += ZM[i][k - 1] * ZM1[k][j];//  /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

            if (ENERGY_DEBUG) {
              printf("%+f: MultiloopB;\n", energy / 100);
            }

            if (STRUCTURE_COUNT) {
              ZM[j][i] += ZM[k - 1][i] * ZM1[j][k];
            }
          }
       
      }
        
      // **************************************************************************
      // Solve Z
      // **************************************************************************
      //delta    = jPairedIn(i, j, bpList);
      Z[i][j] += Z[i][j - 1] /* ROOT_POW(root, delta, runLength)*/;

      if (STRUCTURE_COUNT) {
        Z[j][i] += Z[j - 1][i];
      }

      for (k = i; k < j - MIN_PAIR_DIST; ++k) { 
        // (k, j) is the rightmost base pair in (i, j).
        if (canBasePair[intSequence[k]][intSequence[j]]) {
          energy = canBasePair[intSequence[k]][intSequence[j]] > 2 ? TerminalAU : 0;

          if (ENERGY_DEBUG) {
            printf("%+f: %c-%c == (2 || 3) ? 0 : GUAU_penalty;\n", energy / 100, sequence[k], sequence[j]);
          }
            
          if (k == i) {
            //delta    = numBasePairs[i][j] - numBasePairs[k][j];
            Z[i][j] += ZB[k][j] /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

            if (STRUCTURE_COUNT) {
              Z[j][i] += ZB[j][k];
            }
          } else {
            //delta    = numBasePairs[i][j] - numBasePairs[i][k - 1] - numBasePairs[k][j];
            Z[i][j] += Z[i][k - 1] * ZB[k][j] /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

            if (STRUCTURE_COUNT) {
              Z[j][i] += Z[k - 1][i] * ZB[j][k];
            }
          }
        }
      }
    }
  }
  
  for (i = 0; i < NUM_WINDOWS; ++i) {
    for (j = 1; j <= sequenceLength - WINDOW_SIZE(i) + 1; ++j) {
      solutions[i][j][root] = Z[j][j + WINDOW_SIZE(i) - 1];
    }
  }
  /*for (i = 0; i < NUM_WINDOWS; ++i) {
    for (j = 1; j <= sequenceLength - WINDOW_SIZE(i) + 1; ++j) {
      solutions[i][j][root] = Z[j + WINDOW_SIZE(i) - 1][i];
    }
  }*/

	if (FFTBOR_DEBUG) {
		std::cout << "." << std::flush;
	}
}

void evaluateZNS(int root, dcomplex **Z, dcomplex **ZB, dcomplex **ZM, dcomplex **ZM1, dcomplex ***solutions, dcomplex *rootsOfUnity, char *inputSequence, char *sequence, int *intSequence, int *canBasePair[5], int sequenceLength, int runLength, double RT) {
  int i, j, k, l, r, d, delta;
  double energy;
  
  flushMatrices(Z, ZB, ZM, ZM1, sequenceLength,rootsOfUnity[root]);
    
  if (ENERGY_DEBUG) {
    printf("RT: %f\n", RT / 100);
  }
  
  for (d = MIN_PAIR_DIST + 1; d < sequenceLength; ++d) {
    for (i = 1; i <= sequenceLength - d; ++i) {
      j = i + d;
      
      if (canBasePair[intSequence[i]][intSequence[j]]) {
        // ****************************************************************************
        // Solve ZB 
        // ****************************************************************************
        // In a hairpin, [i + 1, j - 1] unpaired.
        //energy    = hairpinloop(i, j, canBasePair[intSequence[i]][intSequence[j]], intSequence, inputSequence);
        //delta     = numBasePairs[i][j] + jPairedTo(i, j, bpList);
        delta = 0;
        ZB[i][j] += ROOT_POW(root, 1, runLength);// *  exp(-energy / RT);

        if (ENERGY_DEBUG) {
          printf("%+f: GetHairpinEnergy(%c (%d), %c (%d));\n", energy / 100, sequence[i], i, sequence[j], j);
        }

        if (STRUCTURE_COUNT) {
          ZB[j][i] += 1;
        }

        // Interior loop / bulge / stack / multiloop.
        for (l = i + 1; l < min2(i+30,j- MIN_PAIR_DIST-2)+1; ++l) {
          for (r = max2(l + MIN_PAIR_DIST + 1, j - (30 - (l - i))); r < j; ++r) {
            if (canBasePair[intSequence[l]][intSequence[r]]) {
               // In interior loop / bulge / stack with (i, j) and (k, l), (i + 1, k - 1) and (l + 1, j - 1) are all unpaired.
               //energy    = interiorloop(i, j, k, l, canBasePair[intSequence[i]][intSequence[j]], canBasePair[intSequence[l]][intSequence[k]], intSequence);
               //delta     = numBasePairs[i][j] - numBasePairs[k][l] + jPairedTo(i, j, bpList);
               ZB[i][j] += ZB[l][r];// /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);
                 
               if (ENERGY_DEBUG) {
                 printf("%+f: GetInteriorStackingAndBulgeEnergy(%c (%d), %c (%d), %c (%d), %c (%d));\n", energy / 100, sequence[i], i, sequence[j], j, sequence[k], k,
 sequence[l], l);
               }

               if (STRUCTURE_COUNT) {
                 ZB[j][i] += ZB[r][l];
               }
            }
         }
       }
       for(k=i+MIN_PAIR_DIST+3;k<j-4;++k){ 
          // If (i, j) is the closing b.p. of a multiloop, and (k, l) is the rightmost base pair, there is at least one hairpin between (i + 1, k - 1).
          //energy    = multiloop_closing(i, j, k, l, canBasePair[intSequence[j]][intSequence[i]], canBasePair[intSequence[k]][intSequence[l]], intSequence);
          //delta     = numBasePairs[i][j] - numBasePairs[i + 1][k - 1] - numBasePairs[k][l] + jPairedTo(i, j, bpList);
          ZB[i][j] += ZM[i + 1][k - 1] * ZM1[k][j-1];// /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);
                  
          if (ENERGY_DEBUG) {
            printf("%+f: MultiloopA + 2 * MultiloopB + MultiloopC * (%d - %d - 1);\n", energy / 100, j, l);
          }

          if (STRUCTURE_COUNT) {
            ZB[j][i] += ZM[k - 1][i + 1] * ZM1[j-1][k];
          }  
        }
      }

      // ****************************************************************************
      // Solve ZM1
      // ****************************************************************************
      for(k=i+MIN_PAIR_DIST+1;k<j+1;++k){
       if(canBasePair[intSequence[i]][intSequence[k]]){
        //energy = P->MLbase;
        ZM1[i][j] += ZB[i][k];//* exp(-energy/RT);
        if (STRUCTURE_COUNT) {
          ZM1[j][i] += ZB[k][i];
        }
       }
      }
        
      // ****************************************************************************
      // Solve ZM
      // ****************************************************************************
      //energy    = P->MLbase;
      //delta     = jPairedIn(i, j, bpList);
      //ZM[i][j] += ZM[i][j - 1] * /*ROOT_POW(root, delta, runLength) */ exp(-energy / RT);

      /*if (ENERGY_DEBUG) {
        printf("%+f: MultiloopC;\n", energy / 100);
      }

      if (STRUCTURE_COUNT) {
        ZM[j][i] += ZM[j - 1][i];
      }*/

      for (k = i; k < j - MIN_PAIR_DIST; ++k) {
        //if (canBasePair[intSequence[k]][intSequence[j]]) {
          // Only one stem.
          //energy    = /*P->MLintern[canBasePair[intSequence[k]][intSequence[j]]] +*/ P->MLbase * (k - i);
          //delta     = numBasePairs[i][j] - numBasePairs[k][j];
          ZM[i][j] += ZM1[k][j];// /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

          if (ENERGY_DEBUG) {
            printf("%+f: MultiloopB + MultiloopC * (%d - %d);\n", energy / 100, k, i);
          }

          if (STRUCTURE_COUNT) {
            ZM[j][i] += ZM1[j][k];
          }

          // More than one stem.
          if (k >= i + MIN_PAIR_DIST + 2) {
            //energy    = P->MLintern[canBasePair[intSequence[k]][intSequence[j]]];
            //delta     = numBasePairs[i][j] - numBasePairs[i][k - 1] - numBasePairs[k][j];
            ZM[i][j] += ZM[i][k - 1] * ZM1[k][j];//  /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

            if (ENERGY_DEBUG) {
              printf("%+f: MultiloopB;\n", energy / 100);
            }

            if (STRUCTURE_COUNT) {
              ZM[j][i] += ZM[k - 1][i] * ZM1[j][k];
            }
          }
       
      }
        
      // **************************************************************************
      // Solve Z
      // **************************************************************************
      //delta    = jPairedIn(i, j, bpList);
      Z[i][j] += Z[i][j - 1] /* ROOT_POW(root, delta, runLength)*/;

      if (STRUCTURE_COUNT) {
        Z[j][i] += Z[j - 1][i];
      }

      for (k = i; k < j - MIN_PAIR_DIST; ++k) { 
        // (k, j) is the rightmost base pair in (i, j).
        if (canBasePair[intSequence[k]][intSequence[j]]) {
          //energy = canBasePair[intSequence[k]][intSequence[j]] > 2 ? TerminalAU : 0;

          if (ENERGY_DEBUG) {
            printf("%+f: %c-%c == (2 || 3) ? 0 : GUAU_penalty;\n", energy / 100, sequence[k], sequence[j]);
          }
            
          if (k == i) {
            //delta    = numBasePairs[i][j] - numBasePairs[k][j];
            Z[i][j] += ZB[k][j];// /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

            if (STRUCTURE_COUNT) {
              Z[j][i] += ZB[j][k];
            }
          } else {
            //delta    = numBasePairs[i][j] - numBasePairs[i][k - 1] - numBasePairs[k][j];
            Z[i][j] += Z[i][k - 1] * ZB[k][j];// /* ROOT_POW(root, 1, runLength)*/ * exp(-energy / RT);

            if (STRUCTURE_COUNT) {
              Z[j][i] += Z[k - 1][i] * ZB[j][k];
            }
          }
        }
      }
    }
  }
  
  for (i = 0; i < NUM_WINDOWS; ++i) {
    for (j = 1; j <= sequenceLength - WINDOW_SIZE(i) + 1; ++j) {
      solutions[i][j][root] = Z[j][j + WINDOW_SIZE(i) - 1];
    }
  }
  /*for (i = 0; i < NUM_WINDOWS; ++i) {
    for (j = 1; j <= sequenceLength - WINDOW_SIZE(i) + 1; ++j) {
      solutions[i][j][root] = Z[j + WINDOW_SIZE(i) - 1][i];
    }
  }*/

        if (FFTBOR_DEBUG) {
                std::cout << "." << std::flush;
        }
}


void solveSystem(dcomplex ***solutions, char *sequence, int sequenceLength, int runLength) {
  char precisionFormat[20];
  int i, j, k;
  double scalingFactor, sum;
  
  sprintf(precisionFormat, "%%d\t%%.0%df\n", PRECISION ? PRECISION : std::numeric_limits<double>::digits10);
  
  fftw_complex signal[runLength + 1];
  fftw_complex result[runLength + 1];
  
  fftw_plan plan = fftw_plan_dft_1d(runLength + 1, signal, result, FFTW_FORWARD, FFTW_ESTIMATE);
  
  for (i = 0; i < NUM_WINDOWS; ++i) {
    for (j = 1; j <= sequenceLength - WINDOW_SIZE(i) + 1; ++j) {
      sum           = 0;
      scalingFactor = solutions[i][j][0].real();
      
      if (!(MIN_WINDOW_SIZE == WINDOW_SIZE && WINDOW_SIZE == N)) {
        printf("Window size:           %d\n", WINDOW_SIZE(i));
        printf("Window starting index: %d\n", j);
      
        printf("Sequence  (%d, %d): ", j, j + WINDOW_SIZE(i) - 1);
        for (k = j; k <= j + WINDOW_SIZE(i) - 1; k++) {
          printf("%c", sequence[k]);
        }
        printf("\n");
      
        /*printf("Structure (%d, %d): ", j, j + WINDOW_SIZE(i) - 1);
        for (k = j; k <= j + WINDOW_SIZE(i) - 1; k++) {
          printf("%c", structure[k] < 0 ? '.' : (structure[k] > k ? '(' : ')'));
        }
        printf("\n");*/
      }
  
      // For some reason it's much more numerically stable to set the signal via real / imag components separately.
      for (k = 0; k <= runLength; k++) {
        // Convert point-value solutions of Z(root) to 10^PRECISION * Z(root) / Z
        signal[k][FFTW_REAL] = (pow(10, PRECISION) * solutions[i][j][k].real()) / scalingFactor;
        signal[k][FFTW_IMAG] = (pow(10, PRECISION) * solutions[i][j][k].imag()) / scalingFactor;
      }
  
      // Calculate transform, coefficients are in fftw_complex result array.
      fftw_execute(plan);
  
      printf("k\tp(k)\n");
  
      for (k = 0; k <= runLength; k++) {
        // Truncate to user-specified precision, default is 4 and if set to 0, no truncation occurs (dangerous).
        if (PRECISION == 0) {
          solutions[i][j][k] = dcomplex(result[k][FFTW_REAL] / (runLength + 1), 0);
        } else {
          solutions[i][j][k] = dcomplex(pow(10.0, -PRECISION) * static_cast<int>(result[k][FFTW_REAL] / (runLength + 1)), 0);
        }
        if(NS==0){
          sum += solutions[i][j][k].real(); 
          printf(precisionFormat, k, solutions[i][j][k].real());
        } else {
          sum += solutions[i][j][k].real();
          printf(precisionFormat, k, solutions[i][j][k].real()*scalingFactor); 
        }
      }
  
      if (FFTBOR_DEBUG) {
      	printf("Scaling factor (Z{%d, %d}): %.15f\n", j, j + WINDOW_SIZE(i) - 1, scalingFactor);
        std::cout << "Sum: " << sum << std::endl << std::endl;
      }
    }
  }
  
  fftw_destroy_plan(plan);
}

int jPairedTo(int i, int j, int *basePairs) {
  return basePairs[i] == j ? -1 : 1;
}

int jPairedIn(int i, int j, int *basePairs) {
  return basePairs[j] >= i && basePairs[j] < j ? 1 : 0;
}

void populateRemainingRoots(dcomplex ***solutions, int sequenceLength, int runLength, int lastRoot) {
  // Optimization leveraging complementarity of roots of unity.
  int i, j, k, root;
  
  for (i = 0; i < NUM_WINDOWS; ++i) {
    for (j = 1; j <= sequenceLength - WINDOW_SIZE(i) + 1; ++j) {
      root = lastRoot;
  
      if (runLength % 2) {
        k = root - 2;
      } else {
        k = root - 1;
      }
  
      for (; root <= runLength && k > 0; --k, ++root) {
        solutions[i][j][root] = dcomplex(solutions[i][j][k].real(), -solutions[i][j][k].imag());
      }
    }
  }
}

void populateMatrices(dcomplex **Z, dcomplex **ZB, dcomplex **ZM, dcomplex **ZM1, dcomplex ***solutions, dcomplex *rootsOfUnity, int sequenceLength, int runLength) {
  int i, j;
  
  for (i = 0; i < NUM_WINDOWS; ++i) {
    solutions[i] = new dcomplex*[sequenceLength - WINDOW_SIZE(i) + 2];
    
    for (j = 1; j <= sequenceLength - WINDOW_SIZE(i) + 1; ++j) {
      solutions[i][j] = new dcomplex[runLength + 1];
    }
  }
  
  for (i = 0; i <= sequenceLength; ++i) {
    Z[i]               = new dcomplex[sequenceLength + 1];
    ZB[i]              = new dcomplex[sequenceLength + 1];
    ZM[i]              = new dcomplex[sequenceLength + 1];
    ZM1[i]             = new dcomplex[sequenceLength + 1];
    if (i <= runLength) {
      rootsOfUnity[i] = dcomplex(cos(2 * M_PI * i / (runLength + 1)), sin(2 * M_PI * i / (runLength + 1)));
    }
  }
}

void flushMatrices(dcomplex **Z, dcomplex **ZB, dcomplex **ZM, dcomplex **ZM1, int sequenceLength, dcomplex root) {
  int i, j;
  
  for (i = 0; i <= sequenceLength; ++i) {
    for (j = 0; j <= sequenceLength; ++j) {
      if (i > 0 && j > 0 && abs(j - i) <= MIN_PAIR_DIST) {
        Z[i][j] = ONE_C;
        ZB[i][j] = ZERO_C;
      } else {
        Z[i][j] = ZERO_C;
        ZB[i][j] = ZERO_C;
      }
				
      //ZB[i][j] = ZERO_C;
      ZM[i][j] = ZERO_C;
      ZM1[i][j] = ZERO_C;
    }
  }
}

void pf(char *a) {
  // I haven't touched this function -- Evan Senter
  int i,j,d;
  int n = strlen(a);
  int k, l;
  double **Z = (double **) xcalloc(n+1,sizeof(double *));
  double **Zb = (double **) xcalloc(n+1,sizeof(double *));
  double **Zm = (double **) xcalloc(n+1,sizeof(double *));
  double RT = 0.0019872370936902486 * (37 + 273.15) * 100; // 0.01 * (kcal K)/mol
  double energy;
  double expE;
  double tempZ;
  int IJ, JI;

  static int **canBasePair;
  int *intSequence = (int *)xcalloc(n+1,sizeof(int));

  const char *energyfile = "energy.par";
  read_parameter_file(energyfile);

  N = n;

  initializeCanBasePair(canBasePair);
  translateToIntSequence(a,intSequence);

  for (j=0;j<=N;j++) {
    Z[j] = (double *) xcalloc(N+1,sizeof(double));
    Zb[j] = (double *) xcalloc(N+1,sizeof(double));
    Zm[j] = (double *) xcalloc(N+1,sizeof(double));
  }

  /* Initialize Sequences of lengths shorter than or equal to MIN_PAIR_DIST=3
   * will have no structure */

  for (d = 0; d <= MIN_PAIR_DIST; d++) {
    for (i = 1; i <= n - d; i++) {
      j = i + d;
      Z[i][j] = 1.0;
      Zb[i][j] = 0.0;
      Zm[i][j] = 0.0;
    }
  }

  for (d = MIN_PAIR_DIST+1; d <= n-1; d++) {
    for (i = 1; i <= n - d; i++) {
      j = i + d;
      Z[i][j] = 0.0;
      Zb[i][j] = 0.0;
      Zm[i][j] = 0.0;
      IJ = canBasePair[intSequence[i]][intSequence[j]];
      JI = canBasePair[intSequence[j]][intSequence[i]];

      /* Start by filling in Zb, this might be needed for Zm or Z */
      if (IJ) {
	tempZ = 0.0;
	/* Hairpin loop */
	energy = hairpinloop(i, j, IJ, intSequence, a);
	tempZ += exp(-energy/RT);
      
	for (k = i+1; k < j - MIN_PAIR_DIST ; k++)
	  for (l = k + MIN_PAIR_DIST + 1; l < j; l++) {
	    if (canBasePair[intSequence[k]][intSequence[l]]) {
	      /* Interior loop, bulge or stack? */
	      energy = interiorloop(i, j, k, l, IJ, 
				    canBasePair[intSequence[l]][intSequence[k]], intSequence);
	      tempZ += Zb[k][l] * exp(-energy/RT);
	      
	      /* Multi loop, where (i,j) is the closing base pair. */
	      if (k>i+MIN_PAIR_DIST+2) {
		  /* If there is a multiloop with (i,j) as the closing
		   * base pair and (k,l) as the right-most base pair
		   * inside i..j, then there will be at least one
		   * 'hairpin' in the region i+1..k-1, or else it is
		   * not a multloop, but rather an interior loop. 
		   */
		energy = multiloop_closing(i,j,k,l,JI,
				   canBasePair[intSequence[k]][intSequence[l]],intSequence);
		tempZ += Zb[k][l] * Zm[i+1][k-1] *
		  exp(-energy/RT);
	      }
	    }
	  }
	Zb[i][j] = tempZ;
      }

      /* Multi loop, Zm */
      tempZ = 0.0;
      for (k = i; k < j - MIN_PAIR_DIST ; k++)
	for (l = k + MIN_PAIR_DIST + 1; l <= j; l++) {
	  if (canBasePair[intSequence[k]][intSequence[l]]) {
	    /* One stem */
	    energy = P->MLintern[canBasePair[intSequence[k]][intSequence[l]]] + P->MLbase*(k-i+j-l);
	    /* terminal base pair is not GC or CG */
	    //if (canBasePair[intSequence[k]][intSequence[l]]>2)
	    //  energy += TerminalAU;
	    tempZ += Zb[k][l] * exp(-energy/RT);
	    
	    /* More than one stem */
	    if (k>i+MIN_PAIR_DIST+2) {
	      energy = P->MLintern[canBasePair[intSequence[k]][intSequence[l]]] + P->MLbase*(j-l);
	      tempZ += Zb[k][l] * Zm[i][k-1] * exp(-energy/RT);
	    }
	  }
	}
      Zm[i][j] = tempZ;
      
      /* Z */
      /* The unpaired structure has energy 0 */
      tempZ = 1.0;
      for (k = i; k < j - MIN_PAIR_DIST ; k++)
	for (l = k + MIN_PAIR_DIST + 1; l <= j; l++) {
	  if (canBasePair[intSequence[k]][intSequence[l]]) {
	    /* Compute Z */
	    energy = 0.0; 
	    /* Terminal AU */
	    if (canBasePair[intSequence[k]][intSequence[l]]>2)
	    energy += TerminalAU;
	    expE = exp(-energy/RT);
	    if (k==i) {
	      tempZ += Zb[k][l] * expE;
	    }
	    else
	      tempZ += Z[i][k-1] * Zb[k][l] * expE;
	  } /* if (canBasePair[intSequence[k]][intSequence[l]]) */
	} /* for */
      Z[i][j] = tempZ;
    }
  } 
  printf("%.12g\n",Z[1][n]);//,-RT/100.*log(Z[1][n]));
  
  for (j=0;j<=N;j++) {
    free(Z[j]);
    free(Zb[j]);
    free(Zm[j]);
    }
  free(Z);
  free(Zb);
  free(Zm); 
}

/* Number of base pairs in the region i to j in bpList */
int numbp(int i, int j, int *bpList) {
  int n=0;
  int k;
  for (k=i;k<=j;k++)
    if ( k<bpList[k] && bpList[k]<=j )
      n++;
  return n;
}

int bn(char A) {
/* Return the integer corresponding to the given base
   @ = 0  A = 1  C = 2  G = 3  U = 4 */
  if (A=='A')
    return 1;
  else if (A=='C')
    return 2;
  else if (A=='G')
    return 3;
  else if (A=='U')
    return 4;
  else
    return 0;
}

void initializeCanBasePair(int **canBasePair) {
  // A = 1, C = 2, G = 3, U = 4
  canBasePair[1][4] = 5;
  canBasePair[4][1] = 6;
  canBasePair[2][3] = 1;
  canBasePair[3][2] = 2;
  canBasePair[3][4] = 3;
  canBasePair[4][3] = 4;
}

void translateToIntSequence(char *a, int *intSequence) {
  int i;
  intSequence[0] = strlen(a);
  for (i=0;i<intSequence[0];i++)
    intSequence[i+1] = bn(a[i]);
}

void initializeBasePairCounts(int **numBasePairs, int *bpList, int n){
  int d,i,j;
  for (i = 1; i <= n; i++)
    for (j = 1; j <= n; j++)
      numBasePairs[i][j] = 0;
  for (d = MIN_PAIR_DIST+1; d < n; d++) {
    for (i = 1; i <= n - d; i++) {
      j = i + d;
      numBasePairs[i][j] = numbp(i,j,bpList);
    }
  }
}

double hairpinloop(int i, int j, int bp_type, int *intSequence, char *a) {
  // char *a is intended to be 0-indexed.
  double energy;
  energy = ((j-i-1) <= 30) ? P->hairpin[j-i-1] :
    P->hairpin[30]+(P->lxc*log((j-i-1)/30.));
  if ((j-i-1) >3) /* No mismatch for triloops */
    energy += P->mismatchH[bp_type]
      [intSequence[i+1]][intSequence[j-1]];
  else {
    char tl[6]={0}, *ts;
    strncpy(tl, a+i-1, 5);
    if ((ts=strstr(Triloops, tl))) 
      energy += P->Triloop_E[(ts - Triloops)/6];
    if (bp_type>2)
      energy += TerminalAU;
	}
  if ((j-i-1) == 4) { /* check for tetraloop bonus */
    char tl[7]={0}, *ts;
    strncpy(tl, a+i-1, 6);
    if ((ts=strstr(Tetraloops, tl))) 
      energy += P->TETRA_ENERGY[(ts - Tetraloops)/7];
  }
  return energy;
}

double interiorloop(int i, int j, int k, int l, int bp_type1, int bp_type2, int *intSequence) {
  double energy;
  int n1,n2;
  /* Interior loop, bulge or stack? */
  n1 = k-i-1;
  n2 = j-l-1;

  if ( (n1>0) && (n2>0) )
    /* Interior loop, special cases for interior loops of 
     * sizes 1+1, 1+2, 2+1, and 2+2. */
    if ( (n1==1) && (n2==1) )
      energy = P->int11[bp_type1][bp_type2]
	[intSequence[i+1]][intSequence[j-1]];
    else if ( (n1==2) && (n2==1) )
      energy = P->int21[bp_type2][bp_type1]
	[intSequence[j-1]][intSequence[i+1]][intSequence[i+2]];
    else if ( (n1==1) && (n2==2) )
      energy = P->int21[bp_type1][bp_type2]
	[intSequence[i+1]][intSequence[l+1]][intSequence[l+2]];
    else if ( (n1==2) && (n2==2) )
      energy = P->int22[bp_type1][bp_type2]
	  [intSequence[i+1]][intSequence[i+2]][intSequence[l+1]][intSequence[l+2]];
    else
      energy = ((n1+n2<=30)?(P->internal_loop[n1+n2]):
	(P->internal_loop[30]+(P->lxc*log((n1+n2)/30.))))+
	P->mismatchI[bp_type1][intSequence[i+1]][intSequence[j-1]] +
	P->mismatchI[bp_type2][intSequence[l+1]][intSequence[k-1]] +
	min2(MAX_NINIO, abs(n1-n2)*P->F_ninio[2]);
  else if ( (n1>0) || (n2>0) ) {
    /* Bulge */
    energy = ((n2+n1)<=30)?P->bulge[n1+n2]:
      (P->bulge[30]+(P->lxc*log((n1+n2)/30.)));
    if ( (n1+n2)==1 )
      /* A bulge of size one is so small that the base pairs
       * can stack */
      energy += P->stack[bp_type1][bp_type2];
    else {
      if ( bp_type1>2)
	energy += TerminalAU;
      if ( bp_type2>2)
	energy += TerminalAU;
    }
  }
  else { /* n1=n2=0 */
    /* Stacking base pair */
    energy = P->stack[bp_type1][bp_type2];
  }
  return energy;
}

double multiloop_closing(int i, int j, int k, int l, int bp_type1, int bp_type2, int *intSequence) {
  /* Multiloop where (i,j) is the closing base pair */
  double energy;
  energy = P->MLclosing + P->MLintern[bp_type1] /*+ P->MLintern[bp_type2] + P->MLbase*(j-l-1);*/;
  return energy;
}
