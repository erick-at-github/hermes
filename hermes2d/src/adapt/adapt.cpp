// This file is part of Hermes2D.
//
// Hermes2D is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// Hermes2D is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Hermes2D.  If not, see <http://www.gnu.org/licenses/>.

#include "../hermes2d.h"
#include "../common.h"
#include "../limit_order.h"
#include "../solution.h"
#include "../feproblem.h"
#include "../refmap.h"
#include "../quad_all.h"
#include "../matrix.h"
#include "../traverse.h"
#include "../norm.h"
#include "../element_to_refine.h"
#include "../ref_selectors/selector.h"
#include "adapt.h"
#include "../views/scalar_view.h"
#include "../views/order_view.h"


using namespace std;

/* Private constants */
#define HERMES_TOTAL_ERROR_MASK 0x0F ///< A mask which mask-out total error type. Used by Adapt::calc_errors_internal(). \internal
#define HERMES_ELEMENT_ERROR_MASK 0xF0 ///< A mask which mask-out element error type. Used by Adapt::calc_errors_internal(). \internal

Adapt::Adapt(Tuple< Space* > spaces_, Tuple<ProjNormType> proj_norms) : num_act_elems(-1), 
                                                                        have_coarse_solutions(false), have_reference_solutions(false), have_errors(false) 
{
  // sanity check
  if (proj_norms.size() > 0 && spaces_.size() != proj_norms.size()) 
    error("Mismatched numbers of spaces and projection types in Adapt::Adapt().");

  this->num = spaces_.size();

  // sanity checks
  error_if(this->num <= 0, "Too few components (%d), only %d supported.", this->num, H2D_MAX_COMPONENTS);
  error_if(this->num >= H2D_MAX_COMPONENTS, "Too many components (%d), only %d supported.", this->num, H2D_MAX_COMPONENTS);
  for (int i = 0; i < this->num; i++) {
    if (spaces_[i] == NULL) error("spaces[%d] is NULL in Adapt::Adapt().", i);
    this->spaces.push_back(spaces_[i]); 
  }

  // reset values
  memset(errors, 0, sizeof(errors));
  memset(form, 0, sizeof(form));
  memset(ord, 0, sizeof(ord));
  memset(sln, 0, sizeof(sln));
  memset(rsln, 0, sizeof(rsln));

  if (proj_norms.size() > 0) {
    for (int i = 0; i < this->num; i++) {
      switch (proj_norms[i]) {
        case HERMES_L2_NORM: form[i][i] = l2_form<double, scalar>; ord[i][i]  = l2_form<Ord, Ord>; break;
        case HERMES_H1_NORM: form[i][i] = h1_form<double, scalar>; ord[i][i]  = h1_form<Ord, Ord>; break;
        case HERMES_H1_SEMINORM: form[i][i] = h1_semi_form<double, scalar>; ord[i][i]  = h1_semi_form<Ord, Ord>; break;
        case HERMES_HCURL_NORM: form[i][i] = hcurl_form<double, scalar>; ord[i][i]  = hcurl_form<Ord, Ord>; break;
        case HERMES_HDIV_NORM: form[i][i] = hdiv_form<double, scalar>; ord[i][i]  = hdiv_form<Ord, Ord>; break;
        default: error("Unknown projection type in Adapt::Adapt().");
      }
    }
  }
}

Adapt::~Adapt()
{
  for (int i = 0; i < this->num; i++)
    delete [] errors[i];
}

//// adapt /////////////////////////////////////////////////////////////////////////////////////////

bool Adapt::adapt(Tuple<RefinementSelectors::Selector *> refinement_selectors, double thr, int strat, 
            int regularize, double to_be_processed)
{
  error_if(!have_errors, "element errors have to be calculated first, call Adapt::calc_err_est().");
  error_if(refinement_selectors == Tuple<RefinementSelectors::Selector *>(), "selector not provided");
  if (spaces.size() != refinement_selectors.size()) error("Wrong number of refinement selectors.");
  TimePeriod cpu_time;

  //get meshes
  int max_id = -1;
  Mesh* meshes[H2D_MAX_COMPONENTS];
  for (int j = 0; j < this->num; j++) {
    meshes[j] = this->spaces[j]->get_mesh();
    rsln[j]->set_quad_2d(&g_quad_2d_std);
    rsln[j]->enable_transform(false);
    if (meshes[j]->get_max_element_id() > max_id)
      max_id = meshes[j]->get_max_element_id();
  }

  //reset element refinement info
  AUTOLA2_OR(int, idx, max_id + 1, this->num + 1);
  for(int j = 0; j < max_id; j++)
    for(int l = 0; l < this->num; l++)
      idx[j][l] = -1; // element not refined

  double err0_squared = 1000.0;
  double processed_error_squared = 0.0;

  vector<ElementToRefine> elem_inx_to_proc; //list of indices of elements that are going to be processed
  elem_inx_to_proc.reserve(num_act_elems);

  //adaptivity loop
  double error_squared_threshod = -1; //an error threshold that breaks the adaptivity loop in a case of strategy 1
  int num_exam_elem = 0; //a number of examined elements
  int num_ignored_elem = 0; //a number of ignored elements
  int num_not_changed = 0; //a number of element that were not changed
  int num_priority_elem = 0; //a number of elements that were processed using priority queue

  bool first_regular_element = true; //true if first regular element was not processed yet
  int inx_regular_element = 0;
  while (inx_regular_element < num_act_elems || !priority_queue.empty())
  {
    int id, comp, inx_element;

    //get element identification
    if (priority_queue.empty()) {
      id = regular_queue[inx_regular_element].id;
      comp = regular_queue[inx_regular_element].comp;
      inx_element = inx_regular_element;
      inx_regular_element++;
    }
    else {
      id = priority_queue.front().id;
      comp = priority_queue.front().comp;
      inx_element = -1;
      priority_queue.pop();
      num_priority_elem++;
    }
    num_exam_elem++;

    //get info linked with the element
    double err_squared = errors[comp][id];
    Mesh* mesh = meshes[comp];
    Element* e = mesh->get_element(id);

    if (!should_ignore_element(inx_element, mesh, e)) {
      //check if adaptivity loop should end
      if (inx_element >= 0) {
        //prepare error threshold for strategy 1
        if (first_regular_element) {
          error_squared_threshod = thr * err_squared;
          first_regular_element = false;
        }

        // first refinement strategy:
        // refine elements until prescribed amount of error is processed
        // if more elements have similar error refine all to keep the mesh symmetric
        if ((strat == 0) && (processed_error_squared > sqrt(thr) * errors_squared_sum) 
                         && fabs((err_squared - err0_squared)/err0_squared) > 1e-3) break;

        // second refinement strategy:
        // refine all elements whose error is bigger than some portion of maximal error
        if ((strat == 1) && (err_squared < error_squared_threshod)) break;

        if ((strat == 2) && (err_squared < thr)) break;

        if ((strat == 3) &&
          ( (err_squared < error_squared_threshod) ||
          ( processed_error_squared > 1.5 * to_be_processed )) ) break;
      }

      // get refinement suggestion
      ElementToRefine elem_ref(id, comp);
      int current = this->spaces[comp]->get_element_order(id);
      bool refined = refinement_selectors[comp]->select_refinement(e, current, rsln[comp], elem_ref);

      //add to a list of elements that are going to be refined
      if (can_refine_element(mesh, e, refined, elem_ref) ) {
        idx[id][comp] = (int)elem_inx_to_proc.size();
        elem_inx_to_proc.push_back(elem_ref);
        err0_squared = err_squared;
        processed_error_squared += err_squared;
      }
      else {
        debug_log("Element (id:%d, comp:%d) not changed", e->id, comp);
        num_not_changed++;
      }
    }
    else {
      num_ignored_elem++;
    }
  }

  verbose("Examined elements: %d", num_exam_elem);
  verbose(" Elements taken from priority queue: %d", num_priority_elem);
  verbose(" Ignored elements: %d", num_ignored_elem);
  verbose(" Not changed elements: %d", num_not_changed);
  verbose(" Elements to process: %d", elem_inx_to_proc.size());
  bool done = false;
  if (num_exam_elem == 0)
    done = true;
  else if (elem_inx_to_proc.empty())
  {
    warn("None of the elements selected for refinement could be refined. Adaptivity step not successful, returning 'true'.");
    done = true;
  }

  //fix refinement if multimesh is used
  fix_shared_mesh_refinements(meshes, elem_inx_to_proc, idx, refinement_selectors);

  //apply refinements
  apply_refinements(elem_inx_to_proc);

  // in singlemesh case, impose same orders across meshes
  homogenize_shared_mesh_orders(meshes);

  // mesh regularization
  if (regularize >= 0)
  {
    if (regularize == 0)
    {
      regularize = 1;
      warn("Total mesh regularization is not supported in adaptivity. 1-irregular mesh is used instead.");
    }
    for (int i = 0; i < this->num; i++)
    {
      int* parents;
      parents = meshes[i]->regularize(regularize);
      this->spaces[i]->distribute_orders(meshes[i], parents);
      delete [] parents;
    }
  }

  for (int j = 0; j < this->num; j++)
    rsln[j]->enable_transform(true);

  verbose("Refined elements: %d", elem_inx_to_proc.size());
  report_time("Refined elements in: %g s", cpu_time.tick().last());

  //store for the user to retrieve
  last_refinements.swap(elem_inx_to_proc);

  have_errors = false;
  if (strat == 2 && done == true)
    have_errors = true; // space without changes

  // since space changed, assign dofs:
  Space::assign_dofs(this->spaces);

  return done;
}

void Adapt::fix_shared_mesh_refinements(Mesh** meshes, std::vector<ElementToRefine>& elems_to_refine, 
                                        AutoLocalArray2<int>& idx, Tuple<RefinementSelectors::Selector *> refinement_selectors) {
  int num_elem_to_proc = elems_to_refine.size();
  for(int inx = 0; inx < num_elem_to_proc; inx++) {
    ElementToRefine& elem_ref = elems_to_refine[inx];
    int current_quad_order = this->spaces[elem_ref.comp]->get_element_order(elem_ref.id);
    Element* current_elem = meshes[elem_ref.comp]->get_element(elem_ref.id);

    //select a refinement used by all components that share a mesh which is about to be refined
    int selected_refinement = elem_ref.split;
    for (int j = 0; j < this->num; j++)
    {
      if (selected_refinement == H2D_REFINEMENT_H) break; // iso refinement is max what can be recieved
      if (j != elem_ref.comp && meshes[j] == meshes[elem_ref.comp]) { // if a mesh is shared
        int ii = idx[elem_ref.id][j];
        if (ii >= 0) { // and the sample element is about to be refined by another compoment
          const ElementToRefine& elem_ref_ii = elems_to_refine[ii];
          if (elem_ref_ii.split != selected_refinement && elem_ref_ii.split != H2D_REFINEMENT_P) { //select more complicated refinement
            if ((elem_ref_ii.split == H2D_REFINEMENT_ANISO_H || elem_ref_ii.split == H2D_REFINEMENT_ANISO_V) && selected_refinement == H2D_REFINEMENT_P)
              selected_refinement = elem_ref_ii.split;
            else
              selected_refinement = H2D_REFINEMENT_H;
          }
        }
      }
    }

    //fix other refinements according to the selected refinement
    if (selected_refinement != H2D_REFINEMENT_P)
    {
      //get suggested orders for the selected refinement
      const int* suggested_orders = NULL;
      if (selected_refinement == H2D_REFINEMENT_H)
        suggested_orders = elem_ref.q;

      //update orders
      for (int j = 0; j < this->num; j++) {
        if (j != elem_ref.comp && meshes[j] == meshes[elem_ref.comp]) { // if components share the mesh
          // change currently processed refinement
          if (elem_ref.split != selected_refinement) {
            elem_ref.split = selected_refinement;
            refinement_selectors[j]->generate_shared_mesh_orders(current_elem, current_quad_order, elem_ref.split, elem_ref.p, suggested_orders);
          }

          // change other refinements
          int ii = idx[elem_ref.id][j];
          if (ii >= 0) {
            ElementToRefine& elem_ref_ii = elems_to_refine[ii];
            if (elem_ref_ii.split != selected_refinement) {
              elem_ref_ii.split = selected_refinement;
              refinement_selectors[j]->generate_shared_mesh_orders(current_elem, current_quad_order, elem_ref_ii.split, elem_ref_ii.p, suggested_orders);
            }
          }
          else { // element (of the other comp.) not refined at all: assign refinement
            ElementToRefine elem_ref_new(elem_ref.id, j);
            elem_ref_new.split = selected_refinement;
            refinement_selectors[j]->generate_shared_mesh_orders(current_elem, current_quad_order, elem_ref_new.split, elem_ref_new.p, suggested_orders);
            elems_to_refine.push_back(elem_ref_new);
          }
        }
      }
    }
  }
}

void Adapt::homogenize_shared_mesh_orders(Mesh** meshes) {
  Element* e;
  for (int i = 0; i < this->num; i++) {
    for_all_active_elements(e, meshes[i]) {
      int current_quad_order = this->spaces[i]->get_element_order(e->id);
      int current_order_h = H2D_GET_H_ORDER(current_quad_order), current_order_v = H2D_GET_V_ORDER(current_quad_order);

      for (int j = 0; j < this->num; j++)
        if ((j != i) && (meshes[j] == meshes[i])) // components share the mesh
        {
          int quad_order = this->spaces[j]->get_element_order(e->id);
          current_order_h = std::max(current_order_h, H2D_GET_H_ORDER(quad_order));
          current_order_v = std::max(current_order_v, H2D_GET_V_ORDER(quad_order));
        }

      this->spaces[i]->set_element_order_internal(e->id, H2D_MAKE_QUAD_ORDER(current_order_h, current_order_v));
    }
  }
}

const std::vector<ElementToRefine>& Adapt::get_last_refinements() const {
  return last_refinements;
}

void Adapt::apply_refinements(std::vector<ElementToRefine>& elems_to_refine)
{
  for (vector<ElementToRefine>::const_iterator elem_ref = elems_to_refine.begin(); 
       elem_ref != elems_to_refine.end(); elem_ref++) { // go over elements to be refined
    apply_refinement(*elem_ref);
  }
}

void Adapt::apply_refinement(const ElementToRefine& elem_ref) {
  Space* space = this->spaces[elem_ref.comp];
  Mesh* mesh = space->get_mesh();

  Element* e;
  e = mesh->get_element(elem_ref.id);

  if (elem_ref.split == H2D_REFINEMENT_P)
    space->set_element_order_internal(elem_ref.id, elem_ref.p[0]);
  else if (elem_ref.split == H2D_REFINEMENT_H) {
    if (e->active)
      mesh->refine_element(elem_ref.id);
    for (int j = 0; j < 4; j++)
      space->set_element_order_internal(e->sons[j]->id, elem_ref.p[j]);
  }
  else {
    if (e->active)
      mesh->refine_element(elem_ref.id, elem_ref.split);
    for (int j = 0; j < 2; j++)
      space->set_element_order_internal(e->sons[ (elem_ref.split == 1) ? j : j+2 ]->id, elem_ref.p[j]);
  }
}

///// Unrefinements /////////////////////////////////////////////////////////////////////////////////

void Adapt::unrefine(double thr)
{
  if (!have_errors)
    error("Element errors have to be calculated first, see Adapt::calc_err_est().");
  if (this->num > 2) error("Unrefine implemented for two spaces only.");

  Mesh* mesh[2];
  mesh[0] = this->spaces[0]->get_mesh();
  mesh[1] = this->spaces[1]->get_mesh();


  int k = 0;
  if (mesh[0] == mesh[1]) // single mesh
  {
    Element* e;
    for_all_inactive_elements(e, mesh[0])
    {
      bool found = true;
      for (int i = 0; i < 4; i++)
        if (e->sons[i] != NULL && ((!e->sons[i]->active) || (e->sons[i]->is_curved())))
      { found = false;  break; }

      if (found)
      {
        double sum1_squared = 0.0, sum2_squared = 0.0;
        int max1 = 0, max2 = 0;
        for (int i = 0; i < H2D_MAX_ELEMENT_SONS; i++)
          if (e->sons[i] != NULL)
        {
          sum1_squared += errors[0][e->sons[i]->id];
          sum2_squared += errors[1][e->sons[i]->id];
          int oo = this->spaces[0]->get_element_order(e->sons[i]->id);
          if (oo > max1) max1 = oo;
          oo = this->spaces[1]->get_element_order(e->sons[i]->id);
          if (oo > max2) max2 = oo;
        }
        if ((sum1_squared < thr * errors[regular_queue[0].comp][regular_queue[0].id]) &&
             (sum2_squared < thr * errors[regular_queue[0].comp][regular_queue[0].id]))
        {
          mesh[0]->unrefine_element(e->id);
          mesh[1]->unrefine_element(e->id);
          errors[0][e->id] = sum1_squared;
          errors[1][e->id] = sum2_squared;
          this->spaces[0]->set_element_order_internal(e->id, max1);
          this->spaces[1]->set_element_order_internal(e->id, max2);
          k++; // number of unrefined elements
        }
      }
    }
    for_all_active_elements(e, mesh[0])
    {
      for (int i = 0; i < 2; i++)
        if (errors[i][e->id] < thr/4 * errors[regular_queue[0].comp][regular_queue[0].id])
      {
        int oo = H2D_GET_H_ORDER(this->spaces[i]->get_element_order(e->id));
        this->spaces[i]->set_element_order_internal(e->id, std::max(oo - 1, 1));
        k++;
      }
    }
  }
  else // multimesh
  {
    for (int m = 0; m < 2; m++)
    {
      Element* e;
      for_all_inactive_elements(e, mesh[m])
      {
        bool found = true;
        for (int i = 0; i < H2D_MAX_ELEMENT_SONS; i++)
          if (e->sons[i] != NULL && ((!e->sons[i]->active) || (e->sons[i]->is_curved())))
        { found = false;  break; }

        if (found)
        {
          double sum_squared = 0.0;
          int max = 0;
          for (int i = 0; i < 4; i++)
            if (e->sons[i] != NULL)
          {
            sum_squared += errors[m][e->sons[i]->id];
            int oo = this->spaces[m]->get_element_order(e->sons[i]->id);
            if (oo > max) max = oo;
          }
          if ((sum_squared < thr * errors[regular_queue[0].comp][regular_queue[0].id]))
          //if ((sum < 0.1 * thr))
          {
            mesh[m]->unrefine_element(e->id);
            errors[m][e->id] = sum_squared;
            this->spaces[m]->set_element_order_internal(e->id, max);
            k++; // number of unrefined elements
          }
        }
      }
      for_all_active_elements(e, mesh[m])
      {
        if (errors[m][e->id] < thr/4 * errors[regular_queue[0].comp][regular_queue[0].id])
        {
          int oo = H2D_GET_H_ORDER(this->spaces[m]->get_element_order(e->id));
          this->spaces[m]->set_element_order_internal(e->id, std::max(oo - 1, 1));
          k++;
        }
      }
    }
  }
  verbose("Unrefined %d elements.", k);
  have_errors = false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void Adapt::set_error_form(int i, int j, matrix_form_val_t bi_form, matrix_form_ord_t bi_ord)
{
  error_if(i < 0 || i >= this->num || j < 0 || j >= this->num, "invalid component number (%d, %d), max. supported components: %d", i, j, H2D_MAX_COMPONENTS);

  form[i][j] = bi_form;
  ord[i][j] = bi_ord;
}

// case i = j = 0
void Adapt::set_error_form(matrix_form_val_t bi_form, matrix_form_ord_t bi_ord)
{
  int i = 0;
  int j = 0;

  form[i][j] = bi_form;
  ord[i][j] = bi_ord;
}

double Adapt::eval_error(matrix_form_val_t bi_fn, matrix_form_ord_t bi_ord,
                                 MeshFunction *sln1, MeshFunction *sln2, MeshFunction *rsln1, MeshFunction *rsln2)
{
  RefMap *rv1 = sln1->get_refmap();
	RefMap *rv2 = sln1->get_refmap();
	RefMap *rrv1 = rsln1->get_refmap();
	RefMap *rrv2 = rsln1->get_refmap();

  // determine the integration order
  int inc = (rsln1->get_num_components() == 2) ? 1 : 0;
  Func<Ord>* ou = init_fn_ord(rsln1->get_fn_order() + inc);
  Func<Ord>* ov = init_fn_ord(rsln2->get_fn_order() + inc);

  double fake_wt = 1.0;
  Geom<Ord>* fake_e = init_geom_ord();
  Ord o = bi_ord(1, &fake_wt, NULL, ou, ov, fake_e, NULL);
  int order = rrv1->get_inv_ref_order();
  order += o.get_order();
  if(static_cast<Solution *>(rsln1) || static_cast<Solution *>(rsln2))
  {
    if(static_cast<Solution *>(rsln1)->get_type() == Solution::HERMES_EXACT)
    { limit_order_nowarn(order); }
    else
      limit_order(order);
  }
  else  
    limit_order(order);

  ou->free_ord(); delete ou;
  ov->free_ord(); delete ov;
  delete fake_e;

  // eval the form
  Quad2D* quad = sln1->get_quad_2d();
  double3* pt = quad->get_points(order);
  int np = quad->get_num_points(order);

  // init geometry and jacobian*weights
  Geom<double>* e = init_geom_vol(rrv1, order);
  double* jac = rrv1->get_jacobian(order);
  double* jwt = new double[np];
  for(int i = 0; i < np; i++)
    jwt[i] = pt[i][2] * jac[i];

  // function values and values of external functions
  Func<scalar>* err1 = init_fn(sln1, rv1, order);
  Func<scalar>* err2 = init_fn(sln2, rv2, order);
  Func<scalar>* v1 = init_fn(rsln1, rrv1, order);
  Func<scalar>* v2 = init_fn(rsln2, rrv2, order);

  err1->subtract(*v1);
  err2->subtract(*v2);

  scalar res = bi_fn(np, jwt, NULL, err1, err2, e, NULL);

  e->free(); delete e;
  delete [] jwt;
  err1->free_fn(); delete err1;
  err2->free_fn(); delete err2;
  v1->free_fn(); delete v1;
  v2->free_fn(); delete v2;

  return std::abs(res);
}


double Adapt::eval_norm(matrix_form_val_t bi_fn, matrix_form_ord_t bi_ord,
                                MeshFunction *rsln1, MeshFunction *rsln2)
{
  RefMap *rrv1 = rsln1->get_refmap();
	RefMap *rrv2 = rsln1->get_refmap();

  // determine the integration order
  int inc = (rsln1->get_num_components() == 2) ? 1 : 0;
  Func<Ord>* ou = init_fn_ord(rsln1->get_fn_order() + inc);
  Func<Ord>* ov = init_fn_ord(rsln2->get_fn_order() + inc);

  double fake_wt = 1.0;
  Geom<Ord>* fake_e = init_geom_ord();
  Ord o = bi_ord(1, &fake_wt, NULL, ou, ov, fake_e, NULL);
  int order = rrv1->get_inv_ref_order();
  order += o.get_order();
  if(static_cast<Solution *>(rsln1) || static_cast<Solution *>(rsln2))
  {
    if(static_cast<Solution *>(rsln1)->get_type() == Solution::HERMES_EXACT)
    { limit_order_nowarn(order);  }
    else
      limit_order(order);
  }
  else  
    limit_order(order);

  ou->free_ord(); delete ou;
  ov->free_ord(); delete ov;
  delete fake_e;

  // eval the form
  Quad2D* quad = rsln1->get_quad_2d();
  double3* pt = quad->get_points(order);
  int np = quad->get_num_points(order);

  // init geometry and jacobian*weights
  Geom<double>* e = init_geom_vol(rrv1, order);
  double* jac = rrv1->get_jacobian(order);
  double* jwt = new double[np];
  for(int i = 0; i < np; i++)
    jwt[i] = pt[i][2] * jac[i];

  // function values
  Func<scalar>* v1 = init_fn(rsln1, rrv1, order);
  Func<scalar>* v2 = init_fn(rsln2, rrv2, order);

  scalar res = bi_fn(np, jwt, NULL, v1, v2, e, NULL);

  e->free(); delete e;
  delete [] jwt;
  v1->free_fn(); delete v1;
  v2->free_fn(); delete v2;

  return std::abs(res);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
double Adapt::calc_err_internal(Tuple<Solution *> slns, Tuple<Solution *> rslns, unsigned int error_flags, Tuple<double>* component_errors, bool solutions_for_adapt)
{
  _F_
	int i, j, k;

	int n = slns.size();
	if (n != this->num) EXIT("Wrong number of solutions.");

	Timer tmr;
	tmr.start();

  Solution* rslns_original[10];
  Solution* slns_original[10];

	for (i = 0; i < n; i++) {
    slns_original[i] = this->sln[i];
	  this->sln[i] = slns[i];
	  sln[i]->set_quad_2d(&g_quad_2d_std);
	}
	for (i = 0; i < n; i++) {
    rslns_original[i] = this->rsln[i];
	  this->rsln[i] = rslns[i];
	  rsln[i]->set_quad_2d(&g_quad_2d_std);
	}

  have_coarse_solutions = true;
  have_reference_solutions = true;

  // Prepare multi-mesh traversal and error arrays.
  Mesh **meshes = new Mesh *[2 * num];
	Transformable **tr = new Transformable *[2 * num];
	Traverse trav;
	num_act_elems = 0;
	for (i = 0; i < num; i++) {
		meshes[i] = sln[i]->get_mesh();
		meshes[i + num] = rsln[i]->get_mesh();
		tr[i] = sln[i];
		tr[i + num] = rsln[i];

		num_act_elems += sln[i]->get_mesh()->get_num_active_elements();

		int max = meshes[i]->get_max_element_id();
		  if(solutions_for_adapt) {
        if (errors[i] != NULL) delete [] errors[i];
		    errors[i] = new double[max];
		    memset(errors[i], 0, sizeof(double) * max);
      }
	 }

  double total_norm = 0.0;
  double *norms = new double[num];
  memset(norms, 0, num * sizeof(double));
  double *errors_components = new double[num];
  memset(errors_components, 0, num * sizeof(double));
  if(solutions_for_adapt)
    this->errors_squared_sum = 0.0;
  double total_error = 0.0;

  // Calculate error.
  Element **ee;
  trav.begin(2 * num, meshes, tr);
  while ((ee = trav.get_next_state(NULL, NULL)) != NULL) {
    for (i = 0; i < num; i++) {
      for (j = 0; j < num; j++) {
	      if (form[i][j] != NULL) {
		      double err, nrm;
					
          err = fabs(eval_error(form[i][j], ord[i][j], sln[i], sln[j], rsln[i], rsln[j]));
		      nrm = fabs(eval_norm(form[i][j], ord[i][j], rsln[i], rsln[j]));

		      norms[i] += nrm;
		      total_norm  += nrm;
          total_error += err;
          errors_components[i] += err;
          if(solutions_for_adapt)
          {
            this->errors[i][ee[i]->id] += err;
		        this->errors_squared_sum += err;
          }

	      }
      }
    }
  }
  trav.finish();

  // Store the calculation for each solution component separately.
  if(component_errors != NULL) {
    component_errors->clear();
    for (int i = 0; i < num; i++) {
      if((error_flags & HERMES_TOTAL_ERROR_MASK) == HERMES_TOTAL_ERROR_ABS)
        component_errors->push_back(sqrt(errors_components[i]));
      else if ((error_flags & HERMES_TOTAL_ERROR_MASK) == HERMES_TOTAL_ERROR_REL)
        component_errors->push_back(sqrt(errors_components[i]/norms[i]));
      else {
        error("Unknown total error type (0x%x).", error_flags & HERMES_TOTAL_ERROR_MASK);
        return -1.0;
      }
    }
  }

  tmr.stop();
  error_time = tmr.get_seconds();

  // Make the error relative if needed.
  if(solutions_for_adapt) {
    if ((error_flags & HERMES_ELEMENT_ERROR_MASK) == HERMES_ELEMENT_ERROR_REL) {
      for (int i = 0; i < this->num; i++) {
        Element* e;
        for_all_active_elements(e, meshes[i])
          errors[i][e->id] /= norms[i];
      }
    }
    if ((error_flags & HERMES_TOTAL_ERROR_MASK) == HERMES_TOTAL_ERROR_REL) 
      errors_squared_sum = errors_squared_sum / total_norm;
  }

  // Prepare an ordered list of elements according to an error.
  if(solutions_for_adapt) {
    fill_regular_queue(meshes);
    have_errors = true;
  }
  else {
    for (i = 0; i < n; i++) {
      this->sln[i] = slns_original[i];
      this->rsln[i] = rslns_original[i];
    }
	}

  // Return error value.
  if ((error_flags & HERMES_TOTAL_ERROR_MASK) == HERMES_TOTAL_ERROR_ABS)
    return sqrt(total_error);
  else if ((error_flags & HERMES_TOTAL_ERROR_MASK) == HERMES_TOTAL_ERROR_REL)
    return sqrt(total_error / total_norm);
  else {
    error("Unknown total error type (0x%x).", error_flags & HERMES_TOTAL_ERROR_MASK);
    return -1.0;
  }
}

void Adapt::fill_regular_queue(Mesh** meshes) {
  assert_msg(num_act_elems > 0, "Number of active elements (%d) is invalid.", num_act_elems);

  //prepare space for queue (it is assumed that it will only grow since we can just split)
  regular_queue.clear();
  if (num_act_elems < (int)regular_queue.capacity()) {
    vector<ElementReference> empty_refs;
    regular_queue.swap(empty_refs); //deallocate
    regular_queue.reserve(num_act_elems); //allocate
  }

  //prepare initial fill
  Element* e;
  vector<ElementReference>::iterator elem_info = regular_queue.begin();
  for (int i = 0; i < this->num; i++)
    for_all_active_elements(e, meshes[i])
      regular_queue.push_back(ElementReference(e->id, i));

  //sort
  std::sort(regular_queue.begin(), regular_queue.end(), CompareElements(errors));
}