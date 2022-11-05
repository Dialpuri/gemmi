// Copyright 2017-2022 Global Phasing Ltd.

#include <stdio.h>             // for printf, fprintf
#include <cstdlib>             // for getenv
#include <iostream>            // for cerr
#include <stdexcept>           // for exception
#include "gemmi/crd.hpp"       // for prepare_crd, prepare_rst
#include "gemmi/to_cif.hpp"    // for write_cif_block_to_stream
#include "gemmi/fstream.hpp"   // for Ofstream
#include "gemmi/polyheur.hpp"  // for setup_entities
#include "gemmi/monlib.hpp"    // for MonLib, read_monomer_lib
#include "gemmi/read_cif.hpp"  // for read_cif_gz
#include "gemmi/read_coor.hpp" // for read_structure_gz
#include "gemmi/contact.hpp"   // for ContactSearch
#include "gemmi/to_chemcomp.hpp" // for make_chemcomp_with_restraints

#define GEMMI_PROG prep
#include "options.h"

using namespace gemmi;

namespace {

enum OptionIndex {
  Monomers=4, Libin, Libin2, AutoCis, AutoLink, AutoLigand,
  NoZeroOccRestr, NoHydrogens, KeepHydrogens
};

const option::Descriptor Usage[] = {
  { NoOp, 0, "", "", Arg::None,
    "Usage:"
    "\n " EXE_NAME " [options] INPUT_FILE OUTPUT_FILE"
    "\n\nPrepare intermediate Refmac files."
    "\nINPUT_FILE can be in PDB, mmCIF or mmJSON format."
    "\n\nOptions:" },
  CommonUsage[Help],
  CommonUsage[Version],
  CommonUsage[Verbose],
  { Monomers, 0, "", "monomers", Arg::Required,
    "  --monomers=DIR  \tMonomer library dir (default: $CLIBD_MON)." },
  { Libin, 0, "", "lib", Arg::Required,
    "  --lib=CIF  \tUser's library with priority over the monomer library. "
    "Can be given multiple times. If CIF is '+' reads INPUT_FILE (mmCIF only)." },
  { Libin2, 0, "", "low", Arg::Required,
    "  --low=CIF  \tLike --lib, but with the lowest priority." },
  { AutoCis, 0, "", "auto-cis", Arg::YesNo,
    "  --auto-cis=Y|N  \tAssign cis/trans ignoring CISPEP record (default: Y)." },
  { AutoLink, 0, "", "auto-link", Arg::YesNo,
    "  --auto-link=Y|N  \tFind links not included in LINK/SSBOND (default: N)." },
  { AutoLigand, 0, "", "auto-ligand", Arg::YesNo,
    "  --auto-ligand=Y|N  \tFind links not included in LINK/SSBOND (default: N)." },
  //{ NoZeroOccRestr, 0, "", "no-zero-occ", Arg::None,
  //  "  --no-zero-occ  \tNo restraints for zero-occupancy atoms." },
  { NoOp, 0, "", "", Arg::None,
    "\nHydrogen options (default: remove and add on riding positions):" },
  { NoHydrogens, 0, "H", "no-hydrogens", Arg::None,
    "  -H, --no-hydrogens  \tRemove (and do not add) hydrogens." },
  { KeepHydrogens, 0, "", "keep-hydrogens", Arg::None,
    "  --keep-hydrogens  \tPreserve hydrogens from the input file." },
  { 0, 0, 0, 0, 0, 0 }
};

inline const Residue* find_most_complete_residue(const std::string& name,
                                                 const Model& model) {
  const Residue* r = nullptr;
  for (const Chain& chain : model.chains)
    for (const Residue& residue : chain.residues)
      if (residue.name == name && (!r || residue.atoms.size() > r->atoms.size()))
        r = &residue;
  return r;
}

} // anonymous namespace

int GEMMI_MAIN(int argc, char **argv) {
  OptParser p(EXE_NAME);
  p.simple_parse(argc, argv, Usage);
  p.require_positional_args(2);
  p.check_exclusive_pair(KeepHydrogens, NoHydrogens);
  const char* monomer_dir = p.options[Monomers] ? p.options[Monomers].arg
                                                : std::getenv("CLIBD_MON");
  if (monomer_dir == nullptr || *monomer_dir == '\0') {
    fprintf(stderr, "Set $CLIBD_MON or use option --monomers.\n");
    return 1;
  }
  std::string input = p.coordinate_input_file(0);
  std::string output = p.nonOption(1);
  bool verbose = p.options[Verbose];

  try {
    if (verbose)
      printf("Reading %s ...\n", input.c_str());
    cif::Document st_doc;
    Structure st = read_structure_gz(input, CoorFormat::Detect, &st_doc);
    setup_entities(st);

    if (st.models.empty()) {
      fprintf(stderr, "No models found in the input file.\n");
      return 1;
    }
    Model& model0 = st.models[0];

    MonLib monlib;

    auto read_user_file = [&](const char* path) {
      if (verbose)
        printf("Reading user's library %s...\n", path);
      if (path[0] == '+' && path[1] == '\0')
        monlib.read_monomer_doc(st_doc);
      else
        monlib.read_monomer_cif(path, read_cif_gz);
    };

    for (const option::Option* opt = p.options[Libin]; opt; opt = opt->next())
      read_user_file(opt->arg);
    if (verbose && !monlib.monomers.empty()) {
      printf("Monomers read so far:");
      for (auto& mpair : monlib.monomers)
        printf(" %s", mpair.first.c_str());
      putchar('\n');
    }

    std::vector<std::string> needed = model0.get_all_residue_names();
    vector_remove_if(needed, [&](const std::string& s) { return monlib.monomers.count(s); });
    if (verbose)
      printf("Reading monomer library...\n");
    std::string error;
    monlib.read_monomer_lib(monomer_dir, needed, read_cif_gz, &error);
    if (!error.empty())
      printf("%s", error.c_str());
    for (const option::Option* opt = p.options[Libin2]; opt; opt = opt->next())
      read_user_file(opt->arg);
    vector_remove_if(needed, [&](const std::string& s) { return monlib.monomers.count(s); });

    if (!needed.empty()) {
      for (const std::string& name : needed)
        printf("WARNING: definition not found for %s.\n", name.c_str());
      bool make_new_ligands = p.is_yes(AutoLigand, false);
      if (!make_new_ligands)
        fail("Missing monomer definitions");
      for (const std::string& name : needed)
        if (monlib.monomers.find(name) == monlib.monomers.end())
          if (const Residue* res = find_most_complete_residue(name, model0))
            monlib.monomers.emplace(name, make_chemcomp_with_restraints(*res));
      printf("WARNING: Using ad-hoc restraints for missing monomers.\n"
             "WARNING: Restraints generated by a dedicated program would be better.\n");
    }

    if (p.is_yes(AutoCis, true))
      assign_cis_flags(model0);

    if (p.is_yes(AutoLink, false)) {
      size_t before = st.connections.size();
      add_automatic_links(model0, st, monlib);
      if (verbose)
        for (size_t i = before; i < st.connections.size(); ++i) {
          const Connection& conn = st.connections[i];
          printf("Automatic link: %s - %s\n",
                 conn.partner1.str().c_str(), conn.partner2.str().c_str());
        }
    }

    if (verbose)
      printf("Preparing topology, hydrogens, restraints...\n");
    bool reorder = true;
    bool ignore_unknown_links = false;
    HydrogenChange h_change;
    if (p.options[NoHydrogens])
      h_change = HydrogenChange::Remove;
    else if (p.options[KeepHydrogens])
      h_change = HydrogenChange::NoChange;
    else
      h_change = HydrogenChange::ReAddButWater;
    auto topo = prepare_topology(st, monlib, 0, h_change, reorder,
                                 &std::cerr, ignore_unknown_links);
    if (verbose)
      printf("Preparing data for Refmac...\n");
    cif::Document crd = prepare_refmac_crd(st, *topo, monlib, h_change);
    if (verbose)
      printf("Writing %s\n", output.c_str());
    Ofstream os(output);
    write_cif_to_stream(os.ref(), crd, cif::Style::NoBlankLines);
  } catch (std::exception& e) {
    fprintf(stderr, "ERROR: %s\n", e.what());
    return 1;
  }
  return 0;
}