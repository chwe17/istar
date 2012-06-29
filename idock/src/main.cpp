/*

	Copyright (c) 2012, The Chinese University of Hong Kong

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.

*/

#include <iostream>
#include <iomanip>
#include <boost/thread/thread.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <mongo/client/dbclient.h>
#include "seed.hpp"
#include "receptor.hpp"
#include "ligand.hpp"
#include "thread_pool.hpp"
#include "grid_map_task.hpp"
#include "monte_carlo_task.hpp"
#include "summary.hpp"

int main(int argc, char* argv[])
{
	using std::string;
	using std::cout;
	using boost::array;
	using boost::filesystem::ifstream;
	using boost::filesystem::ofstream;
	using namespace boost::program_options;
	using namespace mongo;
	using namespace bson;
	using namespace idock;

	// Create program options.
	string host, db, user, pwd;
	options_description options("input (required)");
	options.add_options()
		("host", value<string>(&host)->required(), "server to connect to")
		("db"  , value<string>(&db  )->required(), "database to login to")
		("user", value<string>(&user)->required(), "username for authentication")
		("pwd" , value<string>(&pwd )->required(), "password for authentication")
		;

	// If no command line argument is supplied, simply print the usage and exit.
	if (argc == 1)
	{
		cout << options;
		return 0;
	}

	// Parse command line arguments.
	variables_map vm;
	store(parse_command_line(argc, argv, options), vm);
	vm.notify();

	// Connect to host.
	DBClientConnection c;
	cout << "Connecting to " << host << '\n';
	c.connect(host);

	// Authenticate user.
	cout << "Authenticating user " << user << '\n';
	string errmsg;
	c.auth(db, user, pwd, errmsg);

	// Initialize the default values of immutable arguments.
	const size_t num_ligands = 12171187;
	const path jobs_path = "jobs";
	const path ligands_path = "16.pdbqt";
	const path headers_path = "16_hdr.bin";
	const path csv_path = "log.csv";
	const size_t num_threads = boost::thread::hardware_concurrency();
	const size_t seed = random_seed();
	const size_t num_mc_tasks = 32;
	const fl energy_range = 3.0;
	const fl grid_granularity = 0.08;
	const directory_iterator end_dir_iter; // A default constructed directory_iterator acts as the end iterator.

	// Initialize the default values of optional arguments.
	const fl default_mwt_lb = 400;
	const fl default_mwt_ub = 500;
	const fl default_logp_lb = -1;
	const fl default_logp_ub = 6;
	const fl default_ad_lb = -50;
	const fl default_ad_ub = 50;
	const fl default_pd_lb = -150;
	const fl default_pd_ub = 0;
	const size_t default_hbd_lb = 1;
	const size_t default_hbd_ub = 6;
	const size_t default_hba_lb = 1;
	const size_t default_hba_ub = 10;
	const size_t default_tpsa_lb = 20;
	const size_t default_tpsa_ub = 80;
	const int64_t default_charge_lb = 0;
	const int64_t default_charge_ub = 0;
	const size_t default_nrb_lb = 2;
	const size_t default_nrb_ub = 9;
	const path default_output_folder_path = "output";
	const size_t max_conformations = 100;
	const size_t max_results = 20; // Maximum number of results obtained from a single Monte Carlo task.

	// Initialize a Mersenne Twister random number generator.
	cout << "Using random seed " << seed << '\n';
	mt19937eng eng(seed);

	// Initialize a thread pool and create worker threads for later use.
	cout << "Creating a thread pool of " << num_threads << " worker threads\n";
	thread_pool tp(num_threads);

	// Precalculate the scoring function in parallel.
	cout << "Precalculating scoring function in parallel\n";
	scoring_function sf;
	{
		// Precalculate reciprocal square root values.
		vector<fl> rs(scoring_function::Num_Samples, 0);
		for (size_t i = 0; i < scoring_function::Num_Samples; ++i)
		{
			rs[i] = sqrt(i * scoring_function::Factor_Inverse);
		}
		BOOST_ASSERT(rs.front() == 0);
		BOOST_ASSERT(rs.back() == scoring_function::Cutoff);

		// Populate the scoring function task container.
		const size_t num_sf_tasks = ((XS_TYPE_SIZE + 1) * XS_TYPE_SIZE) >> 1;
		ptr_vector<packaged_task<void>> sf_tasks(num_sf_tasks);
		for (size_t t1 =  0; t1 < XS_TYPE_SIZE; ++t1)
		for (size_t t2 = t1; t2 < XS_TYPE_SIZE; ++t2)
		{
			sf_tasks.push_back(new packaged_task<void>(boost::bind<void>(&scoring_function::precalculate, boost::ref(sf), t1, t2, boost::cref(rs))));
		}
		BOOST_ASSERT(sf_tasks.size() == num_sf_tasks);

		// Run the scoring function tasks in parallel asynchronously and display the progress bar with hashes.
		tp.run(sf_tasks);

		// Wait until all the scoring function tasks are completed.
		tp.sync();
	}

	// Precalculate alpha values for determining step size in BFGS.
	array<fl, num_alphas> alphas;
	alphas[0] = 1;
	for (size_t i = 1; i < num_alphas; ++i)
	{
		alphas[i] = alphas[i - 1] * 0.1;
	}

	// Initialize a vector of empty grid maps. Each grid map corresponds to an XScore atom type.
	vector<array3d<fl>> grid_maps(XS_TYPE_SIZE);

	// Fetch and execute jobs always
	while (true)
	{
		// Fetch a pending job. Start a transaction, fetch a job where machine == null or Date.now() > time + delta, and update machine and time.
		// Always fetch the same job if not all the slices are done. Use the old _id for query. No need to 1) define box, 2) parse receptor, 3) create grid maps.
		auto cursor = c.query("istar.jobs", QUERY("progress" << 0), 1);
		if (!cursor->more())
		{
			// Sleep for an hour.
			boost::this_thread::sleep(boost::posix_time::hours(1));
			continue;
		}

		auto p = cursor->next();
		const auto _id = p["_id"].String();
		const auto slice = p["slice"].String();
		cout << "Executing job " << _id << '\n';
		c.update("istar.jobs", BSON("_id" << _id << "$atomic" << 1), BSON("$inc" << BSON("progress" << 1)));

		const auto e = c.getLastError();
		if (!e.empty())
		{
			cerr << e << '\n';
		}

		using boost::filesystem::path;
		const path job_path = jobs_path / _id;
		const auto center_x = p["center_x"].Double();
		const auto center_y = p["center_y"].Double();
		const auto center_z = p["center_z"].Double();
		const auto size_x = p["size_x"].Double();
		const auto size_y = p["size_y"].Double();
		const auto size_z = p["size_z"].Double();
		const auto mwt_lb = p["mwt_lb"].ok() ? p["mwt_lb"].Double() : default_mwt_lb;
		const auto mwt_ub = p["mwt_ub"].ok() ? p["mwt_ub"].Double() : default_mwt_ub;
		const auto logp_lb = p["logp_lb"].ok() ? p["logp_lb"].Double() : default_logp_lb;
		const auto logp_ub = p["logp_ub"].ok() ? p["logp_ub"].Double() : default_logp_ub;
		const auto ad_lb = p["ad_lb"].ok() ? p["ad_lb"].Double() : default_ad_lb;
		const auto ad_ub = p["ad_ub"].ok() ? p["ad_ub"].Double() : default_ad_ub;
		const auto pd_lb = p["pd_lb"].ok() ? p["pd_lb"].Double() : default_pd_lb;
		const auto pd_ub = p["pd_ub"].ok() ? p["pd_ub"].Double() : default_pd_ub;
		const auto hbd_lb = p["hbd_lb"].ok() ? p["hbd_lb"].Double() : default_hbd_lb;
		const auto hbd_ub = p["hbd_ub"].ok() ? p["hbd_ub"].Double() : default_hbd_ub;
		const auto hba_lb = p["hba_lb"].ok() ? p["hba_lb"].Double() : default_hba_lb;
		const auto hba_ub = p["hba_ub"].ok() ? p["hba_ub"].Double() : default_hba_ub;
		const auto tpsa_lb = p["tpsa_lb"].ok() ? p["tpsa_lb"].Double() : default_tpsa_lb;
		const auto tpsa_ub = p["tpsa_ub"].ok() ? p["tpsa_ub"].Double() : default_tpsa_ub;
		const auto charge_lb = p["charge_lb"].ok() ? p["charge_lb"].Double() : default_nrb_lb;
		const auto charge_ub = p["charge_ub"].ok() ? p["charge_ub"].Double() : default_nrb_ub;
		const auto nrb_lb = p["nrb_lb"].ok() ? p["nrb_lb"].Double() : default_nrb_lb;
		const auto nrb_ub = p["nrb_ub"].ok() ? p["nrb_ub"].Double() : default_nrb_ub;

		// Fetch a pending slice.
		const size_t slices[101] = { 0, 121712, 243424, 365136, 486848, 608560, 730272, 851984, 973696, 1095408, 1217120, 1338832, 1460544, 1582256, 1703968, 1825680, 1947392, 2069104, 2190816, 2312528, 2434240, 2555952, 2677664, 2799376, 2921088, 3042800, 3164512, 3286224, 3407936, 3529648, 3651360, 3773072, 3894784, 4016496, 4138208, 4259920, 4381632, 4503344, 4625056, 4746768, 4868480, 4990192, 5111904, 5233616, 5355328, 5477040, 5598752, 5720464, 5842176, 5963888, 6085600, 6207312, 6329024, 6450736, 6572448, 6694160, 6815872, 6937584, 7059296, 7181008, 7302720, 7424432, 7546144, 7667856, 7789568, 7911280, 8032992, 8154704, 8276416, 8398128, 8519840, 8641552, 8763264, 8884976, 9006688, 9128400, 9250112, 9371824, 9493536, 9615248, 9736960, 9858672, 9980384, 10102096, 10223808, 10345520, 10467232, 10588944, 10710655, 10832366, 10954077, 11075788, 11197499, 11319210, 11440921, 11562632, 11684343, 11806054, 11927765, 12049476, 12171187 };
		const size_t s = lexical_cast<size_t>(slice);
		const size_t start_lig = slices[s];
		const size_t end_lig = slices[s + 1];

		// Initialize the search space of cuboid shape.
		const box b(vec3(center_x, center_y, center_z), vec3(size_x, size_y, size_z), grid_granularity);

		// Parse the receptor.
		cout << "Parsing receptor\n";
		const receptor rec(p["receptor"].String());

		// Divide the box into coarse-grained partitions for subsequent grid map creation.
		array3d<vector<size_t>> partitions(b.num_partitions);
		{
			// Find all the heavy receptor atoms that are within 8A of the box.
			vector<size_t> receptor_atoms_within_cutoff;
			receptor_atoms_within_cutoff.reserve(rec.atoms.size());
			const size_t num_rec_atoms = rec.atoms.size();
			for (size_t i = 0; i < num_rec_atoms; ++i)
			{
				const atom& a = rec.atoms[i];
				if (b.within_cutoff(a.coordinate))
				{
					receptor_atoms_within_cutoff.push_back(i);
				}
			}
			const size_t num_receptor_atoms_within_cutoff = receptor_atoms_within_cutoff.size();

			// Allocate each nearby receptor atom to its corresponding partition.
			for (size_t x = 0; x < b.num_partitions[0]; ++x)
			for (size_t y = 0; y < b.num_partitions[1]; ++y)
			for (size_t z = 0; z < b.num_partitions[2]; ++z)
			{
				partitions(x, y, z).reserve(num_receptor_atoms_within_cutoff);
				const array<size_t, 3> index1 = {{ x,     y,     z     }};
				const array<size_t, 3> index2 = {{ x + 1, y + 1, z + 1 }};
				const vec3 corner1 = b.partition_corner1(index1);
				const vec3 corner2 = b.partition_corner1(index2);
				for (size_t l = 0; l < num_receptor_atoms_within_cutoff; ++l)
				{
					const size_t i = receptor_atoms_within_cutoff[l];
					if (b.within_cutoff(corner1, corner2, rec.atoms[i].coordinate))
					{
						partitions(x, y, z).push_back(i);
					}
				}
			}
		}

		// Reserve storage for task containers.
		const size_t num_gm_tasks = b.num_probes[0];
		ptr_vector<packaged_task<void>> gm_tasks(num_gm_tasks);
		ptr_vector<packaged_task<void>> mc_tasks(num_mc_tasks);

		// Reserve storage for result containers. ptr_vector<T> is used for fast sorting.
		ptr_vector<ptr_vector<result>> result_containers;
		result_containers.resize(num_mc_tasks);
		for (size_t i = 0; i < num_mc_tasks; ++i)
		{
			result_containers[i].reserve(max_results);
		}
		ptr_vector<result> results;
		results.reserve(max_results * num_mc_tasks);

		vector<size_t> atom_types_to_populate;
		atom_types_to_populate.reserve(XS_TYPE_SIZE);

		cout << "Running " << num_mc_tasks << " Monte Carlo tasks per ligand\n";

		// Perform phase 1 screening, filter ligands, and write zero conformation. Refresh progress every 1%.

		// Skip ligands that have been screened.
		if (exists(csv_path))
		{
			// Obtain last line
			ifstream csv(csv_path);
		}

		string line;
		line.reserve(80);
		ifstream headers(headers_path);
		headers.seekg(sizeof(size_t) * start_lig);
		ifstream ligands(ligands_path);
		ofstream csv(csv_path);
		csv.setf(std::ios::fixed, std::ios::floatfield);
		csv << '\n' << std::setprecision(3);
		for (size_t i = start_lig; i < end_lig; ++i)
		{
			// Locate a ligand.
			size_t header;
			headers.read((char*)&header, sizeof(size_t));
			ligands.seekg(header);

			// Check if the ligand satisfies the filtering conditions.
			getline(ligands, line);
			const auto mwt = right_cast<fl>(line, 21, 28);
			const auto logp = right_cast<fl>(line, 30, 37);
			const auto ad = right_cast<fl>(line, 39, 46);
			const auto pd = right_cast<fl>(line, 48, 55);
			const auto hbd = right_cast<size_t>(line, 57, 59);
			const auto hba = right_cast<size_t>(line, 61, 63);
			const auto tpsa = right_cast<size_t>(line, 65, 67);
			const auto charge = right_cast<int64_t>(line, 69, 71);
			const auto nrb = right_cast<size_t>(line, 73, 75);
			if (!((mwt_lb <= mwt) && (mwt <= mwt_ub) && (logp_lb <= logp) && (logp <= logp_ub) && (ad_lb <= ad) && (ad <= ad_ub) && (pd_lb <= pd) && (pd <= pd_ub) && (hbd_lb <= hbd) && (hbd <= hbd_ub) && (hba_lb <= hba) && (hba <= hba_ub) && (tpsa_lb <= tpsa) && (tpsa <= tpsa_ub) && (charge_lb <= charge) && (charge <= charge_ub) && (nrb_lb <= nrb) && (nrb <= nrb_ub))) continue;

			// Obtain ZINC ID.
			const auto zinc_id = line.substr(10, 8);

			// Parse the ligand.
			ligand lig("");

			// Create grid maps on the fly if necessary.
			BOOST_ASSERT(atom_types_to_populate.empty());
			const vector<size_t> ligand_atom_types = lig.get_atom_types();
			const size_t num_ligand_atom_types = ligand_atom_types.size();
			for (size_t i = 0; i < num_ligand_atom_types; ++i)
			{
				const size_t t = ligand_atom_types[i];
				BOOST_ASSERT(t < XS_TYPE_SIZE);
				array3d<fl>& grid_map = grid_maps[t];
				if (grid_map.initialized()) continue; // The grid map of XScore atom type t has already been populated.
				grid_map.resize(b.num_probes); // An exception may be thrown in case memory is exhausted.
				atom_types_to_populate.push_back(t);  // The grid map of XScore atom type t has not been populated and should be populated now.
			}
			const size_t num_atom_types_to_populate = atom_types_to_populate.size();
			if (num_atom_types_to_populate)
			{
				// Creating grid maps is an intermediate step, and thus should not be dumped to the log file.
				cout << "Creating " << std::setw(2) << num_atom_types_to_populate << " grid map" << ((num_atom_types_to_populate == 1) ? ' ' : 's') << "    " << std::flush;

				// Populate the grid map task container.
				BOOST_ASSERT(gm_tasks.empty());
				for (size_t x = 0; x < num_gm_tasks; ++x)
				{
					gm_tasks.push_back(new packaged_task<void>(boost::bind<void>(grid_map_task, boost::ref(grid_maps), boost::cref(atom_types_to_populate), x, boost::cref(sf), boost::cref(b), boost::cref(rec), boost::cref(partitions))));
				}

				// Run the grid map tasks in parallel asynchronously and display the progress bar with hashes.
				tp.run(gm_tasks);

				// Propagate possible exceptions thrown by grid_map_task().
				for (size_t i = 0; i < num_gm_tasks; ++i)
				{
					gm_tasks[i].get_future().get();
				}

				// Block until all the grid map tasks are completed.
				tp.sync();
				gm_tasks.clear();
				atom_types_to_populate.clear();
			}

			// Populate the Monte Carlo task container.
			BOOST_ASSERT(mc_tasks.empty());
			for (size_t i = 0; i < num_mc_tasks; ++i)
			{
				BOOST_ASSERT(result_containers[i].empty());
				mc_tasks.push_back(new packaged_task<void>(boost::bind<void>(monte_carlo_task, boost::ref(result_containers[i]), boost::cref(lig), eng(), boost::cref(alphas), boost::cref(sf), boost::cref(b), boost::cref(grid_maps))));
			}

			// Run the Monte Carlo tasks in parallel asynchronously.
			tp.run(mc_tasks);

			// Merge results from all the tasks into one single result container.
			BOOST_ASSERT(results.empty());
			const fl required_square_error = static_cast<fl>(4 * lig.num_heavy_atoms); // Ligands with RMSD < 2.0 will be clustered into the same cluster.
			for (size_t i = 0; i < num_mc_tasks; ++i)
			{
				mc_tasks[i].get_future().get();
				ptr_vector<result>& task_results = result_containers[i];
				const size_t num_task_results = task_results.size();
				for (size_t j = 0; j < num_task_results; ++j)
				{
					add_to_result_container(results, static_cast<result&&>(task_results[j]), required_square_error);
				}
				task_results.clear();
			}

			// Block until all the Monte Carlo tasks are completed.
			tp.sync();
			mc_tasks.clear();

			// If no conformation can be found, skip the current ligand and proceed with the next one.
			const size_t num_results = std::min<size_t>(results.size(), max_conformations);
			if (!num_results) // Possible if and only if results.size() == 0 because max_conformations >= 1 is enforced when parsing command line arguments.
			{
				continue;
			}

			// Adjust free energy relative to flexibility.
			result& best_result = results.front();
			best_result.e_nd = best_result.f * lig.flexibility_penalty_factor;

			// Clear the results of the current ligand.
			results.clear();

			// Dump ligand summaries to the csv file.
			csv << zinc_id << ',' << best_result.e_nd << '\n';
		}
		csv.close();
		ligands.close();

		// If all the slices are done, perform phase 2 screening.
		if (!c.query("istar.jobs", QUERY("_id" << _id << "progress" << 100), 1)->more()) continue;

		// Initialize necessary variables for storing ligand summaries.
		ptr_vector<summary> summaries(num_ligands);
		vector<fl> energies;
		energies.reserve(max_conformations);

		// Combine multiple csv and sort.
		for (directory_iterator dir_iter(job_path); dir_iter != end_dir_iter; ++dir_iter)
		{
			const path p = dir_iter->path();
			ifstream in(p); // Parsing starts. Open the file stream as late as possible.
			while (getline(in, line))
			{
			}
			in.close(); // Parsing finishes. Close the file stream as soon as possible.
			summaries.push_back(new summary(p.stem().string(), energies));
			energies.clear();
		}

		// Sort the summaries.
		summaries.sort();

		// Save summaries.

		const path output_folder_path = job_path / "output";
		size_t num_conformations; // Number of conformation to output.

		// Send email with link to /.
		const auto email = p["email"].String();
	}
}
