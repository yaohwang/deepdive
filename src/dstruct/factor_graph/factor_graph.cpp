#include "dstruct/factor_graph/factor_graph.h"
#include "dstruct/factor_graph/factor.h"
#include "io/binary_parser.h"
#include <iostream>

#include <fstream>

// 64-bit big endian to little endian
#define bswap_64(x)                                                            \
  ((((x)&0xff00000000000000ull) >> 56) | (((x)&0x00ff000000000000ull) >> 40) | \
   (((x)&0x0000ff0000000000ull) >> 24) | (((x)&0x000000ff00000000ull) >> 8) |  \
   (((x)&0x00000000ff000000ull) << 8) | (((x)&0x0000000000ff0000ull) << 24) |  \
   (((x)&0x000000000000ff00ull) << 40) | (((x)&0x00000000000000ffull) << 56))

// sort according to id
template <class T>
class idsorter : public std::binary_function<T, T, bool> {
 public:
  inline bool operator()(const T &left, const T &right) {
    return left.id < right.id;
  }
};

bool dd::FactorGraph::is_usable() {
  return this->sorted && this->safety_check_passed;
}

dd::FactorGraph::FactorGraph(long _n_var, long _n_factor, long _n_weight,
                             long _n_edge)
    : n_var(_n_var),
      n_factor(_n_factor),
      n_weight(_n_weight),
      n_edge(_n_edge),
      c_nvar(0),
      c_nfactor(0),
      c_nweight(0),
      n_evid(0),
      n_query(0),
      variables(new RawVariable[_n_var]),
      factors(new RawFactor[_n_factor]),
      weights(new Weight[_n_weight]),
      sorted(false),
      safety_check_passed(false) {}

// Load factor graph from files
// It contains two different mode: original mode and incremental mode.
// The logic for this function is:
// original:
//   1. read variables
//   2. read weights
//   3. sort variables and weights by id
//   4. read factors
// incremental:
//   1. read variables
//   2. read weights
//   3. sort variables and weights by id
//   4. read factors
//   5. sort factors by id
//   6. read edges
void dd::FactorGraph::load(const CmdParser &cmd, const bool is_quiet, int inc) {
  // get factor graph file names from command line arguments
  std::string filename_edges;
  std::string filename_factors;
  std::string filename_variables;
  std::string filename_weights;
  filename_weights = cmd.weight_file;
  filename_variables = cmd.variable_file;
  filename_factors = cmd.factor_file;

  // load variables
  long long n_loaded = read_variables(filename_variables, *this);
  assert(n_loaded == n_var);
  if (!is_quiet) {
    std::cout << "LOADED VARIABLES: #" << n_loaded << std::endl;
    std::cout << "         N_QUERY: #" << n_query << std::endl;
    std::cout << "         N_EVID : #" << n_evid << std::endl;
  }

  // load weights
  n_loaded = read_weights(filename_weights, *this);
  assert(n_loaded == n_weight);
  if (!is_quiet) {
    std::cout << "LOADED WEIGHTS: #" << n_loaded << std::endl;
  }

  read_domains(cmd.domain_file, *this);
  this->sorted = true;

  // load factors
  n_loaded = read_factors(filename_factors, *this);

  assert(n_loaded == n_factor);
  if (!is_quiet) {
    std::cout << "LOADED FACTORS: #" << n_loaded << std::endl;
  }

  this->safety_check();
  assert(this->is_usable() == true);
}

/**
 * Compiles the factor graph into a format that's more appropriate for
 * inference and learning.
 *
 * Since the original factor graph initializes the new factor graph,
 * it also has to transfer the variable, factor, and weight counts,
 * and other statistics as well.
 */
void dd::FactorGraph::compile(CompiledFactorGraph &cfg) {
  cfg.c_nvar = c_nvar;
  cfg.c_nfactor = c_nfactor;
  cfg.c_nweight = c_nweight;

  cfg.n_evid = n_evid;
  cfg.n_query = n_query;

  cfg.stepsize = stepsize;

  long i_edge = 0;

  /*
   * For each factor, put the variables sorted within each factor in an
   * array.
   */
  for (long i = 0; i < n_factor; i++) {
    RawFactor &rf = factors[i];
    rf.n_start_i_vif = i_edge;

    /*
     * Each factor knows how many variables it has. After sorting the variables
     * within the factor by their position, lay the variables in this factor
     * one after another in the vifs array.
     */
    std::sort(rf.tmp_variables.begin(), rf.tmp_variables.end(),
              dd::compare_position);
    for (const VariableInFactor &vif : rf.tmp_variables) {
      cfg.vifs[i_edge] = vif;
      i_edge++;
    }

    /* Also copy the clean factor without the temp crap */
    Factor f(rf);
    cfg.factors[i] = f;
  }
  dprintf("i_edge = %ld n_edge = %ld", i_edge, n_edge);
  assert(i_edge == n_edge);

  i_edge = 0;
  long ntallies = 0;

  /*
   * For each variable, lay the factors sequentially in an array as well.
   */
  for (long i = 0; i < n_var; i++) {
    RawVariable &rv = variables[i];
    // I guess it's only at this point that we're sure tmp_factor_ids won't
    // change since we've fully loaded the graph.
    rv.n_factors = rv.tmp_factor_ids.size();
    rv.n_start_i_factors = i_edge;

    if (rv.domain_type == DTYPE_MULTINOMIAL) {
      rv.n_start_i_tally = ntallies;
      ntallies += rv.cardinality;
    }

    for (const long &fid : rv.tmp_factor_ids) {
      cfg.factor_ids[i_edge] = fid;

      cfg.compact_factors[i_edge].id = factors[fid].id;
      cfg.compact_factors[i_edge].func_id = factors[fid].func_id;
      cfg.compact_factors[i_edge].n_variables = factors[fid].n_variables;
      cfg.compact_factors[i_edge].n_start_i_vif = factors[fid].n_start_i_vif;

      cfg.compact_factors_weightids[i_edge] = factors[fid].weight_id;

      i_edge++;
    }

    /* Also remember to copy the clean variable without the temp crap */
    Variable v(rv);
    cfg.variables[i] = v;
  }

  /* Initialize the InferenceResult array in the end of compilation */
  cfg.infrs->init(cfg.variables, weights);

  assert(i_edge == n_edge);
  /*
   * XXX: Ideally, we don't care about the factor graph anymore at this
   * point, but for consistency, I will update the c_edge variable as well.
   */
  c_edge = i_edge;
  cfg.c_edge = i_edge;
}

dd::CompiledFactorGraph::CompiledFactorGraph(long _n_var, long _n_factor,
                                             long _n_weight, long _n_edge)
    : n_var(_n_var),
      n_factor(_n_factor),
      n_weight(_n_weight),
      n_edge(_n_edge),
      c_nvar(0),
      c_nfactor(0),
      c_nweight(0),
      c_edge(0),
      n_evid(0),
      n_query(0),
      variables(new Variable[_n_var]),
      factors(new Factor[_n_factor]),
      compact_factors(new CompactFactor[_n_edge]),
      compact_factors_weightids(new int[_n_edge]),
      factor_ids(new long[_n_edge]),
      vifs(new VariableInFactor[_n_edge]),
      infrs(new InferenceResult(_n_var, _n_weight)),
      safety_check_passed(false) {}

void dd::CompiledFactorGraph::copy_from(
    const CompiledFactorGraph *const p_other_fg) {
  c_nvar = p_other_fg->c_nvar;
  c_nfactor = p_other_fg->c_nfactor;
  c_nweight = p_other_fg->c_nweight;
  c_edge = p_other_fg->c_edge;

  n_evid = p_other_fg->n_evid;
  n_query = p_other_fg->n_query;

  // copy each member from the given graph
  memcpy(variables, p_other_fg->variables, sizeof(Variable) * n_var);
  memcpy(factors, p_other_fg->factors, sizeof(Factor) * n_factor);

  memcpy(compact_factors, p_other_fg->compact_factors,
         sizeof(CompactFactor) * n_edge);
  memcpy(compact_factors_weightids, p_other_fg->compact_factors_weightids,
         sizeof(int) * n_edge);
  memcpy(factor_ids, p_other_fg->factor_ids, sizeof(long) * n_edge);
  memcpy(vifs, p_other_fg->vifs, sizeof(VariableInFactor) * n_edge);

  sorted = p_other_fg->sorted;
  safety_check_passed = p_other_fg->safety_check_passed;

  infrs->copy_from(*p_other_fg->infrs);
  infrs->ntallies = p_other_fg->infrs->ntallies;
  infrs->multinomial_tallies = new int[p_other_fg->infrs->ntallies];
  for (long i = 0; i < infrs->ntallies; i++) {
    infrs->multinomial_tallies[i] = p_other_fg->infrs->multinomial_tallies[i];
  }
}

long dd::CompiledFactorGraph::get_multinomial_weight_id(
    const VariableValue *assignments, const CompactFactor &fs, long vid,
    long proposal) {
  /**
   * The weight ids are aligned in a continuous region according
   * to the numerical order of variable values.
   * For example, for variable assignment indexes i1, ..., ik with cardinality
   * d1, ..., dk
   * The weight index is
   * (...((((0 * d1 + i1) * d2) + i2) * d3 + i3) * d4 + ...) * dk + ik
   */
  long weight_offset = 0;
  // for each variable in the factor
  for (long i = fs.n_start_i_vif; i < fs.n_start_i_vif + fs.n_variables; ++i) {
    const VariableInFactor &vif = vifs[i];
    const Variable &variable = variables[vif.vid];
    weight_offset *= variable.cardinality;
    weight_offset += variable.get_domain_index(
        (vif.vid == vid) ? proposal : (int)assignments[vif.vid]);
  }

  long weight_id = 0;
  switch (fs.func_id) {
    case FUNC_SPARSE_MULTINOMIAL:
      weight_id = factors[fs.id].weight_ids[weight_offset];
      break;
    case FUNC_MULTINOMIAL:
      weight_id = *(compact_factors_weightids + (&fs - compact_factors)) +
                  weight_offset;
      break;
  }
  return weight_id;
}

void dd::CompiledFactorGraph::update_weight(const Variable &variable) {
  // corresponding factors and weights in a continous region
  CompactFactor *const fs = compact_factors + variable.n_start_i_factors;
  const int *const ws = compact_factors_weightids + variable.n_start_i_factors;
  // for each factor
  for (long i = 0; i < variable.n_factors; i++) {
    // boolean variable
    switch (variable.domain_type) {
      case DTYPE_BOOLEAN: {
        // only update weight when it is not fixed
        if (infrs->weights_isfixed[ws[i]] == false) {
          // stochastic gradient ascent
          // increment weight with stepsize * gradient of weight
          // gradient of weight = E[f|D] - E[f], where D is evidence variables,
          // f is the factor function, E[] is expectation. Expectation is
          // calculated
          // using a sample of the variable.
          infrs->weight_values[ws[i]] +=
              stepsize * (this->template potential<false>(fs[i]) -
                          this->template potential<true>(fs[i]));
        }
        break;
      }
      case DTYPE_MULTINOMIAL: {
        // two weights need to be updated
        // sample with evidence fixed, I0, with corresponding weight w1
        // sample without evidence unfixed, I1, with corresponding weight w2
        // gradient of wd0 = f(I0) - I(w1==w2)f(I1)
        // gradient of wd1 = I(w1==w2)f(I0) - f(I1)
        long wid1 =
            get_multinomial_weight_id(infrs->assignments_evid, fs[i], -1, -1);
        long wid2 =
            get_multinomial_weight_id(infrs->assignments_free, fs[i], -1, -1);
        int equal = (wid1 == wid2);

        if (infrs->weights_isfixed[wid1] == false) {
          infrs->weight_values[wid1] +=
              stepsize * (this->template potential<false>(fs[i]) -
                          equal * this->template potential<true>(fs[i]));
        }

        if (infrs->weights_isfixed[wid2] == false) {
          infrs->weight_values[wid2] +=
              stepsize * (equal * this->template potential<false>(fs[i]) -
                          this->template potential<true>(fs[i]));
        }
        break;
      }

      default:
        abort();
    }
  }
}

bool dd::compare_position(const VariableInFactor &x,
                          const VariableInFactor &y) {
  return x.n_position < y.n_position;
}

void dd::FactorGraph::safety_check() {
  // check whether variables, factors, and weights are stored
  // in the order of their id
  long s = n_var;
  for (long i = 0; i < s; i++) {
    assert(this->variables[i].id == i);
  }
  s = n_factor;
  for (long i = 0; i < s; i++) {
    assert(this->factors[i].id == i);
  }
  s = n_weight;
  for (long i = 0; i < s; i++) {
    assert(this->weights[i].id == i);
  }
  this->safety_check_passed = true;
}
