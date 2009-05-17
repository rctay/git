#include "builtin.h"
#include "cache.h"
#include "xdiff/xdiff.h"
#include "xdiff-interface.h"
#include "parse-options.h"

static const char *const merge_file_usage[] = {
	"git merge-file [options] [-L name1 [-L orig [-L name2]]] [--attribute-path path] file1 orig_file file2",
	NULL
};

static int label_cb(const struct option *opt, const char *arg, int unset)
{
	static int label_count = 0;
	const char **names = (const char **)opt->value;

	if (label_count >= 3)
		return error("too many labels on the command line");
	names[label_count++] = arg;
	return 0;
}

int cmd_merge_file(int argc, const char **argv, const char *prefix)
{
	const char *names[3] = { NULL, NULL, NULL };
	mmfile_t mmfs[3];
	mmbuffer_t result = {NULL, 0};
	xpparam_t xpp = {XDF_NEED_MINIMAL};
	int ret = 0, i = 0, to_stdout = 0;
	int merge_level = XDL_MERGE_ZEALOUS_ALNUM;
	int merge_style = 0, quiet = 0;
	int nongit;
	char *attribute_path = NULL;

	struct option options[] = {
		OPT_BOOLEAN('p', "stdout", &to_stdout, "send results to standard output"),
		OPT_SET_INT(0, "diff3", &merge_style, "use a diff3 based merge", XDL_MERGE_DIFF3),
		OPT_STRING('a', "attribute-path", &attribute_path, "path",
			   "apply work-tree conversion for the path"),
		OPT__QUIET(&quiet),
		OPT_CALLBACK('L', NULL, names, "name",
			     "set labels for file1/orig_file/file2", &label_cb),
		OPT_END(),
	};

	prefix = setup_git_directory_gently(&nongit);
	if (!nongit) {
		/* Read the configuration file */
		git_config(git_xmerge_config, NULL);
		if (0 <= git_xmerge_style)
			merge_style = git_xmerge_style;
	}

	argc = parse_options(argc, argv, options, merge_file_usage, 0);
	if (argc != 3)
		usage_with_options(merge_file_usage, options);
	if (quiet) {
		if (!freopen("/dev/null", "w", stderr))
			return error("failed to redirect stderr to /dev/null: "
				     "%s\n", strerror(errno));
	}

	for (i = 0; i < 3; i++) {
		if (!names[i])
			names[i] = argv[i];
		if (read_mmfile(mmfs + i, argv[i]))
			return -1;
		if (buffer_is_binary(mmfs[i].ptr, mmfs[i].size))
			return error("Cannot merge binary files: %s\n",
					argv[i]);
		if (attribute_path) {
			struct strbuf buf = STRBUF_INIT;
			int st;
			st = convert_to_git(attribute_path,
					    mmfs[i].ptr, mmfs[i].size,
					    &buf, 0);
			if (st) {
				size_t len;

				free(mmfs[i].ptr);
				mmfs[i].ptr = strbuf_detach(&buf, &len);
				mmfs[i].size = len;
			}
		}
	}

	ret = xdl_merge(mmfs + 1, mmfs + 0, names[0], mmfs + 2, names[2],
			&xpp, merge_level | merge_style, &result);

	for (i = 0; i < 3; i++)
		free(mmfs[i].ptr);

	if (ret >= 0 && attribute_path) {
		struct strbuf buf = STRBUF_INIT;
		int st;
		st = convert_to_working_tree(attribute_path,
					     result.ptr, result.size,
					     &buf);
		if (st) {
			size_t len;

			free(result.ptr);
			result.ptr = strbuf_detach(&buf, &len);
			result.size = len;
		}
	}

	if (ret >= 0) {
		const char *filename = argv[0];
		FILE *f = to_stdout ? stdout : fopen(filename, "wb");

		if (!f)
			ret = error("Could not open %s for writing", filename);
		else if (result.size &&
			 fwrite(result.ptr, result.size, 1, f) != 1)
			ret = error("Could not write to %s", filename);
		else if (fclose(f))
			ret = error("Could not close %s", filename);
		free(result.ptr);
	}

	return ret;
}
