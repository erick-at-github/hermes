#define H3D_REPORT_WARN
#define H3D_REPORT_INFO
#define H3D_REPORT_VERBOSE
#include "config.h"
//#include <getopt.h>
#include <hermes3d.h>

// With large K, this is a singularly perturbed problem that exhibits an extremely
// thin and steep boundary layer. Singularly perturbed problems are considered to
// be very difficult, but you'll see that Hermes can solve them easily even for large
// values of K.
//
// PDE: -Laplace u + K*K*u = K*K.
//
// Domain: cube (0, 1) x (0, 1) x (0, 1), see the file singpert-aniso.mesh3d.
//
// BC:  Homogeneous Dirichlet.
//
// The following parameters can be changed:

const int INIT_REF_NUM = 0;             // Number of initial uniform mesh refinements.
const int INIT_REF_NUM_BDY = 4;         // Number of initial mesh refinements towards the boundary.
const int P_INIT_X = 1,
          P_INIT_Y = 1,
          P_INIT_Z = 1;                 // Initial polynomial degree of all mesh elements.
const double THRESHOLD = 0.3;           // Error threshold for element refinement of the adapt(...) function 
                                        // (default) STRATEGY = 0 ... refine elements elements until sqrt(THRESHOLD) 
                                        // times total error is processed. If more elements have similar errors, 
                                        // refine all to keep the mesh symmetric.
                                        // STRATEGY = 1 ... refine all elements whose error is larger
                                        // than THRESHOLD times maximum element error.
const double ERR_STOP = 1.0;            // Stopping criterion for adaptivity (rel. error tolerance between the
                                        // fine mesh and coarse mesh solution in percent).
const int NDOF_STOP = 100000;           // Adaptivity process stops when the number of degrees of freedom grows
                                        // over this limit. This is to prevent h-adaptivity to go on forever.
bool solution_output = true;            // Generate output files (if true).
MatrixSolverType matrix_solver = SOLVER_UMFPACK;  // Possibilities: SOLVER_AMESOS, SOLVER_MUMPS, SOLVER_NOX, 
                                                  // SOLVER_PARDISO, SOLVER_PETSC, SOLVER_UMFPACK.

// Problem parameters.
const double K = 1e2;

// Exact solution
#include "exact_solution.cpp"

// Boundary condition types. 
BCType bc_types(int marker)
{
  return BC_ESSENTIAL;
}

// Essential (Dirichlet) boundary condition values. 
scalar essential_bc_values(int ess_bdy_marker, double x, double y, double z)
{
  return 0;
}

// Weak forms. 
#include "forms.cpp"

// Mesh output.
void out_orders(Space *space, const char *name, int iter)
{
  char fname[1024];
  sprintf(fname, "iter-%s-%d.vtk", name, iter);
  FILE *f = fopen(fname, "w");
  if (f != NULL) {
    VtkOutputEngine vtk(f);
    vtk.out_orders(space, name);
    fclose(f);
  }
  else
    warning("Could not open file '%s' for writing.", fname);
}

// Solution output.
void out_fn(MeshFunction *fn, const char *name, int iter)
{
  char fname[1024];
  sprintf(fname, "iter-%s-%d.vtk", name, iter);
  FILE *f = fopen(fname, "w");
  if (f != NULL) {
    VtkOutputEngine vtk(f);
    vtk.out(fn, name);
    fclose(f);
  }
  else warning("Could not open file '%s' for writing.", fname);
}

int main(int argc, char **args) 
{
  // Load the mesh.
  Mesh mesh;
  H3DReader mloader;
  mloader.load("singpert-aniso.mesh3d", &mesh);

  // Perform initial mesh refinement.
  for (int i=0; i < INIT_REF_NUM; i++) mesh.refine_all_elements(H3D_H3D_H3D_REFT_HEX_XYZ);
  mesh.refine_towards_boundary(1, INIT_REF_NUM_BDY);

  // Create an H1 space with default shapeset.
  H1Space space(&mesh, bc_types, essential_bc_values, Ord3(P_INIT_X, P_INIT_Y, P_INIT_Z));

  // Initialize the weak formulation.
  WeakForm wf;
  wf.add_matrix_form(callback(bilinear_form), HERMES_SYM, HERMES_ANY);
  wf.add_vector_form(linear_form, linear_form_ord, HERMES_ANY);

  // Set exact solution.
  ExactSolution exact(&mesh, sol_exact);

  // DOF and CPU convergence graphs.
  SimpleGraph graph_dof_est, graph_cpu_est, graph_dof_exact, graph_cpu_exact;

  // Time measurement.
  TimePeriod cpu_time;
  cpu_time.tick();

  // Adaptivity loop.
  int as = 1; 
  bool done = false;
  do 
  {
    info("---- Adaptivity step %d:", as);

    // Construct globally refined reference mesh and setup reference space.
    Space* ref_space = construct_refined_space(&space,1 , H3D_H3D_H3D_REFT_HEX_XYZ);
    
    // Assemble the reference problem.
    info("Solving on reference mesh (ndof: %d).", Space::get_num_dofs(ref_space));
    bool is_linear = true;
    DiscreteProblem dp(&wf, ref_space, is_linear);
    SparseMatrix* matrix = create_matrix(matrix_solver);
    Vector* rhs = create_vector(matrix_solver);
    Solver* solver = create_solver(matrix_solver, matrix, rhs);
    dp.assemble(matrix, rhs);
    
    // Time measurement.
    cpu_time.tick();

    // Solve the linear system of the reference problem. If successful, obtain the solution.
    Solution ref_sln(ref_space->get_mesh());
    if(solver->solve()) Solution::vector_to_solution(solver->get_solution(), ref_space, &ref_sln);
    else error ("Matrix solver failed.\n");
    
    // Time measurement.
    cpu_time.tick();

    // Project the fine mesh solution onto the coarse mesh.
    Solution sln(space.get_mesh());
    info("Projecting reference solution on coarse mesh.");
    OGProjection::project_global(&space, &ref_sln, &sln, matrix_solver);

    // Time measurement.
    cpu_time.tick();

    // Output solution and mesh.
    if (solution_output) 
    {
      out_fn(&sln, "sln", as);
      out_orders(&space, "order", as);
    }

    // Skip the visualization time.
    cpu_time.tick(HERMES_SKIP);

    // Calculate element errors and total error estimate.
    info("Calculating error estimate and exact error.");
    Adapt *adaptivity = new Adapt(&space, HERMES_H1_NORM);
    bool solutions_for_adapt = true;
    double err_est_rel = adaptivity->calc_err_est(&sln, &ref_sln, solutions_for_adapt) * 100;

    // Calculate exact error.
    solutions_for_adapt = false;
    double err_exact_rel = adaptivity->calc_err_exact(&sln, &exact, solutions_for_adapt) * 100;

    // Report results.
    printf("ndof_coarse: %d, ndof_fine: %d\n", Space::get_num_dofs(&space), Space::get_num_dofs(ref_space));
    printf("err_est_rel: %g%%, err_exact_rel: %g%%\n", err_est_rel, err_exact_rel);

    // Add entry to DOF and CPU convergence graphs.
    graph_dof_est.add_values(Space::get_num_dofs(&space), err_est_rel);
    graph_dof_est.save("conv_dof_est.dat");
    graph_cpu_est.add_values(cpu_time.accumulated(), err_est_rel);
    graph_cpu_est.save("conv_cpu_est.dat");
    graph_dof_exact.add_values(Space::get_num_dofs(&space), err_exact_rel);
    graph_dof_exact.save("conv_dof_exact.dat");
    graph_cpu_exact.add_values(cpu_time.accumulated(), err_exact_rel);
    graph_cpu_exact.save("conv_cpu_exact.dat");

    // If err_est_rel is too large, adapt the mesh. 
    if (err_est_rel < ERR_STOP) done = true;
    else
    {
      info("Adapting coarse mesh.");
      adaptivity->adapt(THRESHOLD);
    }
    if (Space::get_num_dofs(&space) >= NDOF_STOP) done = true;

    // Clean up.
    delete ref_space->get_mesh();
    delete ref_space;
    delete matrix;
    delete rhs;
    delete solver;
    delete adaptivity;

    // Increase the counter of performed adaptivity steps.
    as++;

  } while (!done);

  return 1;
}