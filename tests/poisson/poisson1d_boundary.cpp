/*
1d Poisson solver test of dccrg with boundaries.

Copyright 2012, 2013, 2014 Finnish Meteorological Institute
Copyright 2014 Ilja Honkonen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License version 3
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "boost/mpi.hpp"
#include "cmath"
#include "cstdint"
#include "cstdlib"
#include "iostream"
#include "vector"

#include "dccrg.hpp"
#include "dccrg_cartesian_geometry.hpp"

#include "poisson_solve.hpp"


using namespace boost::mpi;
using namespace dccrg;
using namespace std;


int Poisson_Cell::transfer_switch = Poisson_Cell::INIT;


double get_solution_value(const double x)
{
	return x*x + 3*x + 4;
}

double get_rhs_value(const double)
{
	return 2;
}

/*
Returns the p-norm of the difference of solution from exact.
*/
template<class Geometry> double get_p_norm(
	const std::vector<uint64_t>& cells,
	const dccrg::Dccrg<Poisson_Cell, Geometry>& grid,
	const double p_of_norm
) {
	double local = 0, global = 0;
	BOOST_FOREACH(const uint64_t cell, cells) {
		Poisson_Cell* data = grid[cell];
		if (data == NULL) {
			std::cerr << __FILE__ << ":" << __LINE__
				<< " No data for last cell " << cell
				<< std::endl;
			abort();
		}

		double analytic_solution = std::numeric_limits<double>::max();
		const boost::array<double, 3> cell_center = grid.geometry.get_center(cell);

		analytic_solution = get_solution_value(cell_center[0]);

		//std::cout << cell_center[0] << " " << analytic_solution << " " << data->solution << std::endl;
		local += std::pow(
			fabs(data->solution - analytic_solution),
			p_of_norm
		);
	}
	MPI_Comm temp = grid.get_communicator();
	MPI_Allreduce(&local, &global, 1, MPI_DOUBLE, MPI_SUM, temp);
	MPI_Comm_free(&temp);
	global = std::pow(global, 1.0 / p_of_norm);

	return global;
}


int main(int argc, char* argv[])
{
	environment env(argc, argv);
	communicator comm;

	float zoltan_version;
	if (Zoltan_Initialize(argc, argv, &zoltan_version) != ZOLTAN_OK) {
	    cerr << "Zoltan_Initialize failed" << endl;
	    return EXIT_FAILURE;
	}

	int success = 0; // > 0 means failure

	double old_norm = std::numeric_limits<double>::max();

	const size_t max_number_of_cells = 512;

	for (size_t cells = 16; cells <= max_number_of_cells; cells *= 2) {

		const double cell_length = 4.0 / cells;

		Dccrg<Poisson_Cell, Cartesian_Geometry> grid;

		const boost::array<uint64_t, 3> grid_length = {{cells + 4, 1, 1}};

		grid.initialize(grid_length, comm, "RANDOM", 0, 0, false, false, false);

		Cartesian_Geometry::Parameters geom_params;
		geom_params.start[0] = -2 - 2 * cell_length;
		geom_params.start[1] =
		geom_params.start[2] = 0;
		geom_params.level_0_cell_length[0] = cell_length;
		geom_params.level_0_cell_length[1] =
		geom_params.level_0_cell_length[2] = 1;
		grid.set_geometry(geom_params);

		grid.balance_load();
		std::vector<uint64_t> all_cells = grid.get_cells();
		std::sort(all_cells.begin(), all_cells.end());
		std::vector<uint64_t> solve_cells, boundary_cells, cells_to_skip;

		// classify local cells
		BOOST_FOREACH(const uint64_t cell, all_cells) {
			dccrg::Types<3>::indices_t cell_indices = grid.mapping.get_indices(cell);

			if (
				cell_indices[0] == 0
				or cell_indices[0] == grid_length[0] - 1
			) {

				cells_to_skip.push_back(cell);

			} else if (
				cell_indices[0] == 1
				or cell_indices[0] == grid_length[0] - 2
			) {

				boundary_cells.push_back(cell);

			} else {

				solve_cells.push_back(cell);

			}
		}

		// initialize
		BOOST_FOREACH(const uint64_t cell, solve_cells) {
			Poisson_Cell* const data = grid[cell];

			if (data == NULL) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " No data for cell " << cell
					<< std::endl;
				abort();
			}

			const boost::array<double, 3> cell_center = grid.geometry.get_center(cell);

			data->rhs = get_rhs_value(cell_center[0]);
			data->solution = 0;
		}
		BOOST_FOREACH(const uint64_t cell, boundary_cells) {
			Poisson_Cell* const data = grid[cell];

			if (data == NULL) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " No data for cell " << cell
					<< std::endl;
				abort();
			}

			const boost::array<double, 3> cell_center = grid.geometry.get_center(cell);

			data->rhs = get_rhs_value(cell_center[0]);
			data->solution = get_solution_value(cell_center[0]);
		}

		Poisson_Solve solver;
		solver.solve(solve_cells, grid, cells_to_skip);

		// check that parallel solutions with more cells have smaller norms
		const double
			p_of_norm = 2,
			norm  = get_p_norm(solve_cells, grid, p_of_norm);

		if (norm > old_norm) {
			success = 1;
			if (comm.rank() == 0) {
				std::cerr << __FILE__ << ":" << __LINE__
					<< " " << p_of_norm
					<< "-norm between x and analytic is too large with "
					<< cells  << " cells: " << norm
					<< ", should be <= " << old_norm
					<< std::endl;
			}
			break;
		}

		old_norm = norm;
	}

	if (success == 0) {
		if (comm.rank() == 0) {
			cout << "PASSED" << endl;
		}
		return EXIT_SUCCESS;
	} else {
		if (comm.rank() == 0) {
			cout << "FAILED" << endl;
		}
		return EXIT_FAILURE;
	}
}

