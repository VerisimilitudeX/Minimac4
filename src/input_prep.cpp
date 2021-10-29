#include "input_prep.hpp"
#include "recombination.hpp"

#include <algorithm>
#include <sys/stat.h>

bool stat_tar_panel(const std::string& tar_file_path, std::vector<std::string>& sample_ids)
{
  savvy::reader temp_rdr(tar_file_path);
  if (!temp_rdr)
    return std::cerr << "Error: could not open target file\n", false;

  sample_ids = temp_rdr.samples();

  return true;
}

bool stat_ref_panel(const std::string& ref_file_path, std::string& chrom, std::uint64_t& end_pos)
{
  std::string separate_s1r_path = ref_file_path + ".s1r";
  struct stat st;
  std::vector<savvy::s1r::index_statistics> s1r_stats = savvy::s1r::stat_index(stat(separate_s1r_path.c_str(), &st) == 0 ? separate_s1r_path : ref_file_path);
  if (s1r_stats.size())
  {
    if (chrom.size())
    {
      for (auto it = s1r_stats.begin(); it != s1r_stats.end(); ++it)
      {
        if (it->contig == chrom)
        {
          end_pos = std::min(end_pos, std::uint64_t(it->max_position));
          return true;
        }
      }

      std::cerr << "Error: reference file does not contain chromosome " << chrom << "\n";
      return false;
    }
    else if (s1r_stats.size() == 1)
    {
      chrom = s1r_stats.front().contig;
      end_pos = std::min(end_pos, std::uint64_t(s1r_stats.front().max_position));
      return true;
    }

    std::cerr << "Error: reference file contains multiple chromosomes so --region is required\n";
    return false;
  }
  else if (stat((ref_file_path + ".csi").c_str(), &st) == 0 || stat((ref_file_path + ".tbi").c_str(), &st) == 0)
  {
    savvy::reader stat_rdr(ref_file_path);
    if (chrom.empty())
    {
      savvy::variant var;
      stat_rdr >> var;
      chrom = var.chromosome();
    }

    if (chrom.size())
    {
      for (auto it = stat_rdr.headers().begin(); it != stat_rdr.headers().end(); ++it)
      {
        if (it->first == "contig" && chrom == savvy::parse_header_sub_field(it->second, "ID"))
        {
          std::string length_str = savvy::parse_header_sub_field(it->second, "length");
          if (length_str.size())
          {
            end_pos = std::min(end_pos, std::uint64_t(std::atoll(length_str.c_str())));
            return true;
          }
          break;
        }
      }
      std::cerr << "Error: could not parse chromosome length from VCF/BCF header so --region is required" << std::endl;
      return false;
    }

    std::cerr << "Error: could not determine chromosome from reference" << std::endl;
    return false;
  }

  std::cerr << "Error: could not load reference file index (reference must be an indexed MVCF)\n";
  std::cerr << "Notice: M3VCF files must be updated to an MVCF encoded file. This can be done by running `minimac4 --update-m3vcf input.m3vcf.gz > output.msav`\n";
  return false;
}

bool load_target_haplotypes(const std::string& file_path, const savvy::genomic_region& reg, float error_param, float recom_min, std::vector<target_variant>& target_sites, std::vector<std::string>& sample_ids)
{
  savvy::reader input(file_path);
  if (!input)
    return std::cerr << "Error: cannot open target file\n", false;

  sample_ids = input.samples();
  input.reset_bounds(reg);
  if (!input)
    return std::cerr << "Error: cannot query region (" << reg.chromosome() << ":" << reg.from() << "-" << reg.to() << ") from target file. Target file must be indexed.\n", false;

  savvy::variant var;
  std::vector<std::int8_t> tmp_geno;
  while (input >> var)
  {
    var.get_format("GT", tmp_geno);
    for (std::size_t i = 0; i < var.alts().size(); ++i)
    {
      std::size_t allele_idx = i + 1;
      target_sites.push_back({var.chromosome(), var.position(), var.ref(), var.alts()[i], true, false, std::numeric_limits<float>::quiet_NaN(), error_param, recom_min, {}});
      if (var.alts().size() == 1)
        tmp_geno.swap(target_sites.back().gt);
      else
      {
        target_sites.back().gt.resize(tmp_geno.size());
        std::int8_t* dest_ptr = target_sites.back().gt.data();
        for (std::size_t j = 0; j < tmp_geno.size(); ++j)
          dest_ptr[j] = std::int8_t(tmp_geno[j] == allele_idx);
      }
    }
  }

  return !input.bad();
}

bool load_reference_haplotypes(const std::string& file_path,
  const savvy::genomic_region& extended_reg,
  const savvy::genomic_region& impute_reg,
  const std::unordered_set<std::string>& subset_ids,
  std::vector<target_variant>& target_sites,
  reduced_haplotypes& typed_only_reference_data,
  reduced_haplotypes& full_reference_data)
{
  savvy::reader input(file_path);

  if (input)
  {
    if (!input.reset_bounds(extended_reg, savvy::bounding_point::any))
      return std::cerr << "Error: reference file must be indexed MVCF\n", false;

    bool is_m3vcf_v3 = false;
    for (auto it = input.headers().begin(); !is_m3vcf_v3 && it != input.headers().end(); ++it)
    {
      if (it->first == "subfileformat" && (it->second == "M3VCFv3.0" || it->second == "MVCFv3.0"))
        is_m3vcf_v3 = true;
    }

    if (!is_m3vcf_v3)
      return std::cerr << "Error: reference file must be an MVCF\n", false;

    if (subset_ids.size() && input.subset_samples(subset_ids).empty())
      return std::cerr << "Error: no reference samples overlap subset IDs\n", false;

    savvy::variant var;
    if (!input.read(var))
      return std::cerr << "Notice: no variant records in reference query region (" << extended_reg.chromosome() << ":" << extended_reg.from() << "-" << extended_reg.to() << ")\n", input.bad() ? false : true;

    std::vector<std::int8_t> tmp_geno;
    unique_haplotype_block block;
    auto tar_it = target_sites.begin();
    int res;
    while ((res = block.deserialize(input, var)) > 0)
    {
      if (block.variants().empty() || block.variants().front().pos > extended_reg.to())
        break;

      for (auto ref_it = block.variants().begin(); ref_it != block.variants().end(); ++ref_it)
      {
        while (tar_it != target_sites.end() && tar_it->pos < ref_it->pos)
          ++tar_it;

        for (auto it = tar_it; it != target_sites.end() && it->pos == ref_it->pos; ++it)
        {
          if (it->ref == ref_it->ref && it->alt == ref_it->alt)
          {
            tmp_geno.resize(block.unique_map().size());
            for (std::size_t i = 0; i < tmp_geno.size(); ++i)
              tmp_geno[i] = ref_it->gt[block.unique_map()[i]];

            typed_only_reference_data.compress_variant({it->chrom, it->pos, it->ref, it->alt}, tmp_geno);

            it->af = std::accumulate(tmp_geno.begin(), tmp_geno.end(), 0.f) / tmp_geno.size();
            it->af = float((--typed_only_reference_data.end())->ac) / tmp_geno.size();
            it->in_ref = true;

            if (it != tar_it)
              std::swap(*it, *tar_it);

            ++tar_it;
            break;
          }
        }
      }

      block.trim(impute_reg.from(), impute_reg.to());
      if (!block.variants().empty())
        full_reference_data.append_block(block);
    }

    if (res < 0)
      return false;


    return !input.bad();
  }
  else
  {
    shrinkwrap::gz::istream input_file(file_path);
    std::string line;

    std::uint8_t m3vcf_version = 0;
    const std::string m3vcf_version_line = "##fileformat=M3VCF";
    while (std::getline(input_file, line))
    {
      if (line.substr(0, m3vcf_version_line.size()) == m3vcf_version_line)
      {
        if (line == "##fileformat=M3VCFv2.0")
          m3vcf_version = 2;
        else
          m3vcf_version = 1;
        break;
      }

      if (line.size() < 2 || line[1] != '#')
      {
        std::cerr << "Error: invalid reference file" << std::endl;
        return false;
      }
    }

    std::size_t n_samples = 0;
    while (std::getline(input_file, line))
    {
      if (line.size() < 2 || line[1] != '#')
      {
        n_samples = std::count(line.begin(), line.end(), '\t') - 8;
        break;
      }
    }

    auto tar_it = target_sites.begin();
    unique_haplotype_block block;
    std::vector<std::int8_t> tmp_geno;
    while (block.deserialize(input_file, m3vcf_version, m3vcf_version == 1 ? n_samples : 2 * n_samples))
    {
      if (block.variants().empty() || block.variants().front().pos > extended_reg.to())
        break;

      for (auto ref_it = block.variants().begin(); ref_it != block.variants().end(); ++ref_it)
      {
        while (tar_it != target_sites.end() && tar_it->pos < ref_it->pos)
          ++tar_it;

        for (auto it = tar_it; it != target_sites.end() && it->pos == ref_it->pos; ++it)
        {
          if (it->ref == ref_it->ref && it->alt == ref_it->alt)
          {
            tmp_geno.resize(block.unique_map().size());
            for (std::size_t i = 0; i < tmp_geno.size(); ++i)
              tmp_geno[i] = ref_it->gt[block.unique_map()[i]];

            typed_only_reference_data.compress_variant({it->chrom, it->pos, it->ref, it->alt}, tmp_geno);

            it->af = std::accumulate(tmp_geno.begin(), tmp_geno.end(), 0.f) / tmp_geno.size();
            it->af = float((--typed_only_reference_data.end())->ac) / tmp_geno.size();
            it->in_ref = true;

            if (it != tar_it)
              std::swap(*it, *tar_it);

            ++tar_it;
            break;
          }
        }
      }

      block.trim(impute_reg.from(), impute_reg.to());
      if (!block.variants().empty())
        full_reference_data.append_block(block);
    }
  }

  return !input.bad();
}

std::vector<target_variant> separate_target_only_variants(std::vector<target_variant>& target_sites)
{
  std::vector<target_variant> target_only_sites;
  std::size_t shift_idx = 0;
  for (std::size_t i = 0; i < target_sites.size(); ++i)
  {
    if (!target_sites[i].in_ref)
    {
      target_only_sites.emplace_back();
      std::swap(target_only_sites.back(), target_sites[i]);
    }
    else
    {
      if (i != shift_idx)
      {
        std::swap(target_sites[i], target_sites[shift_idx]);
      }
      ++shift_idx;
    }
  }

  target_sites.resize(shift_idx);
  return target_only_sites;
}

std::vector<std::vector<std::vector<std::size_t>>> generate_reverse_maps(const reduced_haplotypes& typed_only_reference_data)
{
  std::vector<std::vector<std::vector<std::size_t>>> reverse_maps;

  reverse_maps.reserve(typed_only_reference_data.blocks().size());
  for (auto it = typed_only_reference_data.blocks().begin(); it != typed_only_reference_data.blocks().end(); ++it)
  {
    reverse_maps.emplace_back();
    auto& map = reverse_maps.back();
    for (std::size_t i = 0; i < it->cardinalities().size(); ++i)
    {
      map.emplace_back();
      map.back().reserve(it->cardinalities()[i]);
    }

    for (std::size_t i = 0; i < it->unique_map().size(); ++i)
    { assert(it->unique_map()[i] < map.size());
      map[it->unique_map()[i]].push_back(i);
    }
  }

  return reverse_maps;
}

bool convert_old_m3vcf(const std::string& input_path, const std::string& output_path, const std::string& map_file_path)
{
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> ids;

  shrinkwrap::gz::istream input_file(input_path);
  std::string line;

  bool phasing_header_present = false;
  bool contig_header_present = false;
  std::uint8_t m3vcf_version = 0;
  const std::string m3vcf_version_line = "##fileformat=M3VCF";
  const std::string vcf_version_line = "##fileformat=VCF";
  while (std::getline(input_file, line))
  {
    std::size_t equal_pos = line.find('=');
    if (equal_pos != std::string::npos)
    {
      std::string key = line.substr(0, equal_pos);
      std::string val = line.substr(equal_pos + 1);
      key.erase(0, key.find_first_not_of('#'));

      if (key == "fileformat")
      {
        if (val.substr(0, 5) == "M3VCF")
        {
          if (val == "M3VCFv2.0")
            m3vcf_version = 2;
          else
            m3vcf_version = 1;
        }
      }
      else if (key != "INFO" && key != "FORMAT")
      {
        if (!phasing_header_present && key == "phasing")
          phasing_header_present = true;
        else if (!contig_header_present && key == "contig")
          contig_header_present = true;

        headers.emplace_back(std::move(key), std::move(val));
      }
    }
    else
    {

      break;
    }
  }

  if (line.size() < 1 || line[0] != '#')
  {
    std::cerr << "Error: invalid reference file" << std::endl;
    return false;
  }

  headers.insert(headers.begin(), {"subfileformat","M3VCFv3.0"});
  headers.insert(headers.begin(), {"fileformat","VCFv4.2"});
  if (!phasing_header_present)
    headers.emplace_back("phasing","full");
  headers.emplace_back("INFO", "<ID=AC,Number=1,Type=Integer,Description=\"Total number of alternate alleles in called genotypes\">");
  headers.emplace_back("INFO", "<ID=AN,Number=1,Type=Float,Description=\"Total number of alleles in called genotypes\">");
  headers.emplace_back("INFO","<ID=REPS,Number=1,Type=Integer,Description=\"Number of distinct haplotypes in block\">");
  headers.emplace_back("INFO","<ID=VARIANTS,Number=1,Type=Integer,Description=\"Number of variants in block\">");
  headers.emplace_back("INFO","<ID=ERR,Number=1,Type=Integer,Description=\"Error parameter for HMM\">");
//  headers.emplace_back("INFO","<ID=RECOM,Number=1,Type=Integer,Description=\"Recombination probability\">");
  headers.emplace_back("INFO","<ID=CM,Number=1,Type=Integer,Description=\"Centimorgan\">");
  headers.emplace_back("INFO","<ID=END,Number=1,Type=Integer,Description=\"End position of record\">");
  headers.emplace_back("INFO","<ID=UHA,Number=.,Type=Integer,Description=\"Unique haplotype alleles\">");
  headers.emplace_back("FORMAT","<ID=UHM,Number=.,Type=Integer,Description=\"Unique haplotype mapping\">");


  //headers.emplace_back("ALT","<ID=DUP,Description=\"Duplication\">");


  std::size_t tab_cnt = 0;
  std::size_t last_pos = 0;
  std::size_t tab_pos = 0;
  while ((tab_pos = line.find('\t', tab_pos)) != std::string::npos)
  {
    if (tab_cnt >= 9)
    {
      ids.emplace_back(line.substr(last_pos, tab_pos - last_pos));
    }
    last_pos = ++tab_pos;
    ++tab_cnt;
  }

  ids.emplace_back(line.substr(last_pos, tab_pos - last_pos));
  std::size_t n_samples = ids.size();

  unique_haplotype_block block;
  block.deserialize(input_file, m3vcf_version, m3vcf_version == 1 ? n_samples : 2 * n_samples);
  if (!contig_header_present && block.variants().size())
    headers.emplace_back("contig","<ID=" + block.variants()[0].chrom + ">");

  std::string last_3;
  if (output_path.size() >= 3)
    last_3 = output_path.substr(output_path.size() - 3);
  savvy::writer output_file(output_path, last_3 == "bcf" ? savvy::file::format::bcf : savvy::file::format::sav, headers, ids, 6);


  std::size_t block_cnt = 0;

  std::unique_ptr<genetic_map_file> map_file;
  if (!map_file_path.empty() && !block.variants().empty())
  {
    map_file.reset(new genetic_map_file(map_file_path, block.variants()[0].chrom));
    if (!map_file->good())
      return std::cerr << "Error: could not open map file\n", false;
  }

  std::vector<std::int8_t> tmp_geno;
  do
  {
    if (block.variants().empty())
      break;

    if (map_file)
      block.fill_cm(*map_file);

    if (!block.serialize(output_file))
      return false;
    ++block_cnt;
  } while (block.deserialize(input_file, m3vcf_version, m3vcf_version == 1 ? n_samples : 2 * n_samples));

  return !input_file.bad() && output_file.good();
}