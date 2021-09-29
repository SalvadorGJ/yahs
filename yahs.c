/*********************************************************************************
 * MIT License                                                                   *
 *                                                                               *
 * Copyright (c) 2021 Chenxi Zhou <chnx.zhou@gmail.com>                          *
 *                                                                               *
 * Permission is hereby granted, free of charge, to any person obtaining a copy  *
 * of this software and associated documentation files (the "Software"), to deal *
 * in the Software without restriction, including without limitation the rights  *
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell     *
 * copies of the Software, and to permit persons to whom the Software is         *
 * furnished to do so, subject to the following conditions:                      *
 *                                                                               *
 * The above copyright notice and this permission notice shall be included in    *
 * all copies or substantial portions of the Software.                           *
 *                                                                               *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR    *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,      *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE   *
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER        *
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, *
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE *
 * SOFTWARE.                                                                     *
 *********************************************************************************/

/********************************** Revision History *****************************
 *                                                                               *
 * 22/06/21 - Chenxi Zhou: Created                                               *
 *                                                                               *
 *********************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "ketopt.h"
#include "sdict.h"
#include "link.h"
#include "graph.h"
#include "break.h"
#include "asset.h"

#undef DEBUG
#undef DEBUG_ERROR_BREAK
#undef DEBUG_GRAPH_PRUNE
#undef DEBUG_OPTIONS

#define YAHS_VERSION "1.0-rc.3"

static int ec_resolution = 10000;
static int ec_bin = 1000;
static int ec_move_avg = 0;
static int ec_merge_thresh = 10000;
static int ec_dual_break_thresh = 50000;
static double ec_min_frac = 0.80;
static double ec_fold_thresh = 0.1;

int VERBOSE = 0;

graph_t *build_graph_from_links(inter_link_mat_t *link_mat, asm_dict_t *dict, double min_norm)
{
    int32_t i, j, n, c0, c1;
    int8_t t;
    double norm;
    inter_link_t *link;
    graph_t *g;
    graph_arc_t *arc;

    g = graph_init();
    g->sdict = dict;

    // build graph
    n = link_mat->n;
    for (i = 0; i < n; ++i) {
        link = &link_mat->links[i];
        if (link->n == 0)
            continue;
        c0 = link->c0;
        c1 = link->c1;
        t = link->linkt;
        if (!t)
            continue;

        for (j = 0; j < 4; ++j) {
            if (1 << j & t) {
                norm = link->norms[j];
                if (norm >= min_norm) {
                    arc = graph_add_arc(g, c0<<1|j>>1, c1<<1|j&1, -1, 0, norm);
                    graph_add_arc(g, c1<<1|!(j&1), c0<<1|!(j>>1), arc->link_id, 0, norm);
                }
            }
        }
    }

    graph_arc_sort(g);
    graph_arc_index(g);

    return g;
}

int run_scaffolding(char *fai, char *agp, char *link_file, char *out, int resolution)
{
    //TODO: adjust wt thres by resolution
    sdict_t *sdict = make_sdict_from_index(fai);
    asm_dict_t *dict = agp? make_asm_dict_from_agp(sdict, agp) : make_asm_dict_from_sdict(sdict);
    
    int i;
    long len = 0;
    for (i = 0; i < dict->n; ++i)
        len += dict->s[i].len;

#ifdef DEBUG_GRAPH_PRUNE
    printf("[I::%s] #sequences loaded %d = %ldbp\n", __func__, dict->n, len);
#endif

    intra_link_mat_t *intra_link_mat = intra_link_mat_from_file(link_file, dict, resolution, 0);
    norm_t *norm = calc_norms(intra_link_mat);
    if (norm == 0) {
        fprintf(stderr, "[W::%s] no enough bands for norm calculation. Using scaffolds. \n", __func__);
        intra_link_mat_destroy(intra_link_mat);
        intra_link_mat = intra_link_mat_from_file(link_file, dict, resolution, 1);
        norm = calc_norms(intra_link_mat);

        if (norm == 0) {
            intra_link_mat_destroy(intra_link_mat);
            asm_destroy(dict);
            sd_destroy(sdict);
            return 1;
        }
    }

    inter_link_mat_t *inter_link_mat = inter_link_mat_from_file(link_file, dict, resolution, norm->r);
    inter_link_norms(inter_link_mat, norm, 0);
    calc_link_directs(inter_link_mat, 0.1, dict);

    graph_t *g = build_graph_from_links(inter_link_mat, dict, 0.01);

#ifdef DEBUG_GRAPH_PRUNE
    printf("[I::%s] scaffolding graph\n", __func__);
    graph_print(g, stdout, 1);
#endif

    uint64_t n_arc;
    n_arc = g->n_arc;
#ifdef DEBUG_GRAPH_PRUNE
    printf("[I::%s] number edges before trimming: %ld\n", __func__, n_arc);
    int round = 0;
#endif
    while (1) {
        trim_graph_simple_filter(g, 0.1, 0.7, 0);
        trim_graph_tips(g);
        trim_graph_blunts(g);
        trim_graph_repeats(g);
        trim_graph_transitive_edges(g);
        trim_graph_pop_bubbles(g);
        trim_graph_pop_undirected(g);
        trim_graph_weak_edges(g);
        trim_graph_self_loops(g);
#ifdef DEBUG_GRAPH_PRUNE
        printf("[I::%s] number edges after trimming round %d: %ld\n", __func__, ++round, g->n_arc);
#endif
        if (g->n_arc == n_arc)
            break;
        else
            n_arc = g->n_arc;
    }
    trim_graph_ambiguous_edges(g);

#ifdef DEBUG_GRAPH_PRUNE
    printf("[I::%s] scaffolding graph in GV format\n", __func__);
    graph_print_gv(g, stdout);
    printf("[I::%s] scaffolding graph in GFA format\n", __func__);
    graph_print(g, stdout, 1);
#endif

    search_graph_path(g, g->sdict, out);

    intra_link_mat_destroy(intra_link_mat);
    inter_link_mat_destroy(inter_link_mat);
    norm_destroy(norm);
    graph_destroy(g);
    asm_destroy(dict);
    sd_destroy(sdict);

    return 0;
}

int contig_error_break(char *fai, char *link_file, char *out)
{
    int i, ec_round, bp_n, err_no;
    sdict_t *sdict = make_sdict_from_index(fai);
    asm_dict_t *dict;

    int dist_thres = estimate_dist_thres_from_file(link_file, sdict, ec_min_frac, ec_resolution);
    dist_thres = MIN(dist_thres, 1000000);
#ifdef DEBUG_ERROR_BREAK
    printf("[I::%s] dist threshold for error break: %d\n", __func__, dist_thres);
#endif
    char* out1 = (char *) malloc(strlen(out) + 35);
    ec_round = err_no = 0;
    while (1) {
        dict = ec_round? make_asm_dict_from_agp(sdict, out1) : make_asm_dict_from_sdict(sdict);
        link_mat_t *link_mat = link_mat_from_file(link_file, dict, dist_thres, ec_bin, ec_move_avg);
#ifdef DEBUG_ERROR_BREAK
        printf("[I::%s] ec_round %d link matrix\n", __func__, ec_round);
        print_link_mat(link_mat, dict, stdout);
#endif
        bp_n = 0;
        bp_t *breaks = detect_break_points(link_mat, ec_bin, ec_merge_thresh, ec_fold_thresh, ec_dual_break_thresh, &bp_n);
        sprintf(out1, "%s_%02d.agp", out, ++ec_round);
        FILE *agp_out = fopen(out1, "w");
        write_break_agp(dict, breaks, bp_n, agp_out);
        fclose(agp_out);
        
        link_mat_destroy(link_mat);
        asm_destroy(dict);
        for (i = 0; i < bp_n; ++i)
            free(breaks[i].p);
        free(breaks);
        
        err_no += bp_n;
#ifdef DEBUG_ERROR_BREAK
        printf("[I::%s] bp_n %d\n", __func__, bp_n);
#endif
        if (!bp_n)
            break;
    }
    sd_destroy(sdict);
    free(out1);

    fprintf(stderr, "[I::%s] perforem %d round assembly error correction. Made %d breaks \n", __func__, ec_round, err_no);

    return ec_round;
}

int scaffold_error_break(char *fai, char *link_file, char *agp, int flank_size, char *out)
{
    sdict_t *sdict = make_sdict_from_index(fai);
    asm_dict_t *dict = make_asm_dict_from_agp(sdict, agp);

    int dist_thres = flank_size * 2;
    link_mat_t *link_mat = link_mat_from_file(link_file, dict, dist_thres, ec_bin, ec_move_avg);

#ifdef DEBUG_ERROR_BREAK
    printf("[I::%s] dist threshold for error break: %d\n", __func__, dist_thres);
    printf("[I::%s] link matrix\n", __func__);
    print_link_mat(link_mat, dict, stdout);
#endif

    int bp_n = 0;
    bp_t *breaks = detect_break_points_local_joint(link_mat, ec_bin, ec_fold_thresh, flank_size, dict, &bp_n);
    FILE *agp_out = fopen(out, "w");
    write_break_agp(dict, breaks, bp_n, agp_out);
    fclose(agp_out);

    link_mat_destroy(link_mat);
    asm_destroy(dict);
    sd_destroy(sdict);
    int i;
    for (i = 0; i < bp_n; ++i)
        free(breaks[i].p);
    free(breaks);
    
    return bp_n;
}

static void make_agp_from_fai(char *fai, char *out)
{
    int i;
    sdict_t *sdict = make_sdict_from_index(fai);
    FILE *agp_out;
    agp_out = fopen(out, "w");
    if (agp_out == NULL) {
        fprintf(stderr, "[E::%s] cannot open file %s for writing\n", __func__, out);
        exit(EXIT_FAILURE);
    }
    
    for (i = 0; i < sdict->n; ++i)
        fprintf(agp_out, "scaffold_%u\t1\t%u\t1\tW\t%s\t1\t%u\t+\n", i + 1, sdict->s[i].len, sdict->s[i].name, sdict->s[i].len);

    fclose(agp_out);
}

int run_yahs(char *fai, char *agp, char *link_file, char *out, int *resolutions, int nr, int no_contig_ec, int no_scaffold_ec)
{
    char *out_fn = (char *) malloc(strlen(out) + 35);
    char *out_agp = (char *) malloc(strlen(out) + 35);
    char *out_agp_break = (char *) malloc(strlen(out) + 35);
    
    int ec_round, re;
    if (agp == 0 && no_contig_ec == 0) {
        sprintf(out_agp_break, "%s_inital_break", out);
        ec_round = contig_error_break(fai, link_file, out_agp_break);
        sprintf(out_agp_break, "%s_inital_break_%02d.agp", out, ec_round);
    } else {
        if (agp != 0) {
            sprintf(out_agp_break, "%s", agp);
        } else {
            sprintf(out_agp_break, "%s_no_break.agp", out);
            make_agp_from_fai(fai, out_agp_break);
        }
    }

    int r = 0;
    while (r++ < nr) {
        fprintf(stderr, "[I::%s] scaffolding round %d resolution = %d\n", __func__, r, resolutions[r - 1]);
        sprintf(out_fn, "%s_r%02d", out, r);
        re = run_scaffolding(fai, out_agp_break, link_file, out_fn, resolutions[r - 1]);
        if (!re) {
            sprintf(out_agp, "%s_r%02d.agp", out, r);
            if (no_scaffold_ec == 0) {
                sprintf(out_agp_break, "%s_r%02d_break.agp", out, r);
                scaffold_error_break(fai, link_file, out_agp, resolutions[r - 1], out_agp_break);
            } else {
                sprintf(out_agp_break, "%s", out_agp);
            }
        }
    }

    sprintf(out_agp, "%s_scaffolds_final.agp", out);
    // output sorted agp by scaffold size instead of file copy
    // file_copy(out_agp_break, out_agp);
    sdict_t *sdict = make_sdict_from_index(fai);
    asm_dict_t *dict = make_asm_dict_from_agp(sdict, out_agp_break);
    FILE *fo;
    fo = fopen(out_agp, "w");
    if (fo == NULL) {
        fprintf(stderr, "[E::%s] cannot open file %s for writing\n", __func__, out_agp);
        exit(EXIT_FAILURE);
    }
    write_sorted_agp(dict, fo);
    fclose(fo);
    asm_destroy(dict);
    sd_destroy(sdict);

    free(out_agp);
    free(out_fn);
    free(out_agp_break);

    return 0;

}

static int default_resolutions[9] = {50000, 100000, 200000, 500000, 1000000, 2000000, 5000000, 10000000, 20000000};

static int default_nr(char *fai)
{
    int i, max_res, nr;
    int64_t genome_size;
    genome_size = 0;
    sdict_t *sdict = make_sdict_from_index(fai);
    for (i = 0; i < sdict->n; ++i)
        genome_size += sdict->s[i].len;
    sd_destroy(sdict);
    
    max_res = 0;
    if (genome_size < 1000000000)
        max_res = 2000000;
    else if (genome_size < 2000000000)
        max_res = 5000000;
    else if (genome_size < 5000000000)
        max_res = 10000000;
    else
        max_res = 20000000;
    
    nr = 0;
    while (nr < sizeof(default_resolutions) / sizeof(int) && default_resolutions[nr] <= max_res)
        ++nr;

    return nr;
}

static void print_help(FILE *fp_help)
{
    fprintf(fp_help, "Usage: yahs [options] <contigs.fa> <hic.bed>|<hic.bam>|<hic.bin>\n");
    fprintf(fp_help, "Options:\n");
    fprintf(fp_help, "    -a FILE           AGP file (for rescaffolding) [none]\n");
    fprintf(fp_help, "    -r INT[,INT,...]  list of resolutions in ascending order [automate]\n");
    fprintf(fp_help, "    -o STR            prefix of output files [yahs.out]\n");
    fprintf(fp_help, "    -v INT            verbose level [%d]\n", VERBOSE);
    fprintf(fp_help, "    --version         show version number\n");
}

static ko_longopt_t long_options[] = {
    { "no-contig-ec",   ko_no_argument, 301 },
    { "no-scaffold-ec", ko_no_argument, 302 },
    { "help",           ko_no_argument, 'h' },
    { "version",        ko_no_argument, 'V' },
    { 0, 0, 0 }
};

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_help(stderr);
        return 1;
    }

    char *fa, *fai, *agp, *link_file, *out, *restr, *ext, *link_bin_file, *agp_final, *fa_final;
    int *resolutions;
    int nr;
    int no_contig_ec, no_scaffold_ec;

    const char *opt_str = "a:r:o:Vv:h";
    ketopt_t opt = KETOPT_INIT;

    int c, ret;
    FILE *fp_help = stderr;
    fa = fai = agp = link_file = out = restr = link_bin_file = agp_final = fa_final = 0;
    no_contig_ec = no_scaffold_ec = 0;

    while ((c = ketopt(&opt, argc, argv, 1, opt_str, long_options)) >= 0) {
        if (c == 'a') {
            agp = opt.arg;
        } else if (c == 'r') {
            restr = opt.arg;
        } else if (c == 'o') {
            out = opt.arg;
        } else if (c == 301) {
            no_contig_ec = 1;            
        } else if (c == 302) {
            no_scaffold_ec = 1;
        } else if (c == 'v') {
            VERBOSE = atoi(opt.arg);
        } else if (c == 'V') {
            puts(YAHS_VERSION);
            return 0;
        } else if (c == 'h') {
            fp_help = stdout;
        } else if (c == '?') {
            fprintf(stderr, "[E::%s] unknown option: \"%s\"\n", __func__, argv[opt.i - 1]);
            return 1;
        } else if (c == ':') {
            fprintf(stderr, "[E::%s] missing option: \"%s\"\n", __func__, argv[opt.i - 1]);
            return 1;
        }
    }

    if (fp_help == stdout) {
        print_help(stdout);
        return 0;
    }

    if (argc - opt.ind < 2) {
        fprintf(stderr, "[E::%s] missing input: two positional options required\n", __func__);
        print_help(stderr);
        return 1;
    }

    fa = argv[opt.ind];
    link_file = argv[opt.ind + 1];

    fai = (char *) malloc(strlen(fa) + 5);
    sprintf(fai, "%s.fai", fa);

    if (agp)
        no_contig_ec = 1;
    
    if (restr) {
        int max_n_res = 128;
        char  *eptr, *fptr;
        resolutions = (int *) malloc(max_n_res * sizeof(int));
        nr = 0;
        resolutions[nr++] = strtol(restr, &eptr, 10);
        while (*eptr != '\0') {
            if (nr == max_n_res) {
                fprintf(stderr, "[E::%s] more than %d resolutions specified. Is that really necessary?\n", __func__, max_n_res);
                exit(EXIT_FAILURE);                
            }
            resolutions[nr++] = strtol(eptr + 1, &fptr, 10);
            eptr = fptr;
        }
    } else {
        resolutions = default_resolutions;
        nr = default_nr(fai);
    }

    if (out == 0)
        out = "yahs.out";

    ext = link_file + strlen(link_file) - 4;
    if (strcmp(ext, ".bam") == 0) {
        link_bin_file = malloc(strlen(out) + 5);
        sprintf(link_bin_file, "%s.bin", out);
        fprintf(stderr, "[I::%s] dump hic links to binary file %s\n", __func__, link_bin_file);
        dump_links_from_bam_file(link_file, fai, link_bin_file);
    } else if (strcmp(ext, ".bed") == 0) {
        link_bin_file = malloc(strlen(out) + 5);
        sprintf(link_bin_file, "%s.bin", out);
        fprintf(stderr, "[I::%s] dump hic links to binary file %s\n", __func__, link_bin_file);
        dump_links_from_bed_file(link_file, fai, link_bin_file);
    } else if (strcmp(ext, ".bin") == 0) {
        link_bin_file = malloc(strlen(link_file) + 1);
        sprintf(link_bin_file, "%s", link_file);
    } else {
        fprintf(stderr, "[E::%s] unknown link file format. File extension .bam, .bed or .bin is expected\n", __func__);
        exit(EXIT_FAILURE);
    }

#ifdef DEBUG_OPTIONS
    printf("[I::%s] list of options:\n", __func__);
    printf("[I::%s] fa:    %s\n", __func__, fa);
    printf("[I::%s] link:  %s\n", __func__, link_file);
    printf("[I::%s] linkb: %s\n", __func__, link_bin_file);
    printf("[I::%s] agp:   %s\n", __func__, agp);
    printf("[I::%s] res:   %s\n", __func__, restr);
    printf("[I::%s] nr:    %d\n", __func__, nr);
    int i;
    for (i = 0; i < nr; ++i)
        printf("[I::%s] nr=%d:  %d\n", __func__, i, resolutions[i]);
    printf("[I::%s] out:   %s\n", __func__, out);
    printf("[I::%s] ec[C]: %d\n", __func__, no_contig_ec);
    printf("[I::%s] ec[S]: %d\n", __func__, no_scaffold_ec);
#endif

    ret = run_yahs(fai, agp, link_bin_file, out, resolutions, nr, no_contig_ec, no_scaffold_ec);
    
    if (ret == 0) {
        agp_final = (char *) malloc(strlen(out) + 35);
        fa_final = (char *) malloc(strlen(out) + 35);
        sprintf(agp_final, "%s_scaffolds_final.agp", out);
        sprintf(fa_final, "%s_scaffolds_final.fa", out);
        fprintf(stderr, "[I::%s] writing FASTA file for scaffolds\n", __func__);
        FILE *fo;
        fo = fopen(fa_final, "w");
        if (fo == NULL) {
            fprintf(stderr, "[E::%s] cannot open file %s for writing\n", __func__, fa_final);
            exit(EXIT_FAILURE);
        }
        write_fasta_file_from_agp(fa, agp_final, fo, 60);
        fclose(fo);
    }

    if (fai)
        free(fai);

    if (restr)
        free(resolutions);

    if (link_bin_file)
        free(link_bin_file);
    
    if (fa_final)
        free(fa_final);

    if (agp_final)
        free(agp_final);

    return ret;
}
