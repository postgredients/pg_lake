/*
 * Copyright 2025 Snowflake Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Functions for parsing/deparsing COPY data formats.
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "common/string.h"
#include "lib/stringinfo.h"
#include "pg_lake/copy/copy_format.h"
#include "pg_lake/extensions/pg_lake_engine.h"
#include "pg_lake/util/string_utils.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

/* mapping of format name to enum */
typedef struct CopyDataFormatName
{
	char	   *name;
	CopyDataFormat format;
}			CopyDataFormatName;

static CopyDataFormatName DataFormatNames[] =
{
	{
		"csv", DATA_FORMAT_CSV
	},
	{
		"json", DATA_FORMAT_JSON
	},
	{
		"parquet", DATA_FORMAT_PARQUET
	},
	{
		"iceberg", DATA_FORMAT_ICEBERG
	},
#if PG_LAKE_DELTA_SUPPORT == 1
	{
		"delta", DATA_FORMAT_DELTA
	},
#endif
	{
		"gdal", DATA_FORMAT_GDAL
	},
	{
		"log", DATA_FORMAT_LOG
	},
	{
		NULL, DATA_FORMAT_INVALID
	},
};

/* mapping of compression name to enum */
typedef struct CopyDataCompressionName
{
	char	   *name;
	CopyDataCompression compression;
}			CopyDataCompressionName;

static CopyDataCompressionName DataCompressionNames[] =
{
	{
		"none", DATA_COMPRESSION_NONE
	},
	{
		"gzip", DATA_COMPRESSION_GZIP
	},
	{
		"zstd", DATA_COMPRESSION_ZSTD
	},
	{
		"snappy", DATA_COMPRESSION_SNAPPY
	},
	{
		"zip", DATA_COMPRESSION_ZIP
	},
	{
		NULL, DATA_COMPRESSION_INVALID
	},
};

/* mapping of file extension to format and compression */
typedef struct CopyDataFormatExtension
{
	char	   *extension;

	CopyDataFormat format;
	CopyDataCompression compression;
}			CopyDataFormatExtension;

static CopyDataFormatExtension DataFormatExtensions[] =
{
	{
		".csv", DATA_FORMAT_CSV, DATA_COMPRESSION_NONE
	},
	{
		".csv.gz", DATA_FORMAT_CSV, DATA_COMPRESSION_GZIP
	},
	{
		".csv.zst", DATA_FORMAT_CSV, DATA_COMPRESSION_ZSTD
	},
	/* before .json, to take priority in URLToCopyDataFormat */
	{
		".metadata.json", DATA_FORMAT_ICEBERG, DATA_COMPRESSION_NONE
	},
	{
		".json", DATA_FORMAT_JSON, DATA_COMPRESSION_NONE
	},
	{
		".json.gz", DATA_FORMAT_JSON, DATA_COMPRESSION_GZIP
	},
	{
		".json.zst", DATA_FORMAT_JSON, DATA_COMPRESSION_ZSTD
	},
	{
		".parquet", DATA_FORMAT_PARQUET, DATA_COMPRESSION_INVALID
	},
	{
		".geojson", DATA_FORMAT_GDAL, DATA_COMPRESSION_NONE
	},
	{
		".geojson.gz", DATA_FORMAT_GDAL, DATA_COMPRESSION_GZIP
	},
	{
		".gpkg", DATA_FORMAT_GDAL, DATA_COMPRESSION_NONE
	},
	{
		".gpkg.gz", DATA_FORMAT_GDAL, DATA_COMPRESSION_GZIP
	},
	{
		".fgb", DATA_FORMAT_GDAL, DATA_COMPRESSION_NONE
	},
	{
		".kml", DATA_FORMAT_GDAL, DATA_COMPRESSION_NONE
	},
	{
		".kmz", DATA_FORMAT_GDAL, DATA_COMPRESSION_ZIP
	},
	{
		".zip", DATA_FORMAT_GDAL, DATA_COMPRESSION_ZIP
	},
	{
		".dxf", DATA_FORMAT_GDAL, DATA_COMPRESSION_NONE
	},
	{
		".gml", DATA_FORMAT_GDAL, DATA_COMPRESSION_NONE
	},
	{
		NULL, DATA_FORMAT_INVALID, DATA_COMPRESSION_INVALID
	},
};

static char *GetBaseURL(char *url);

/*
 * CopyDataFormatToName returns the name of a copy data format.
 *
 * We could be clever here and use the enum as an index, but the
 * arrays are small and looping is a bit more robust to future
 * changes.
 */
const char *
CopyDataFormatToName(CopyDataFormat format)
{
	const char *name = NULL;

	for (int fmtIndex = 0; DataFormatNames[fmtIndex].name != NULL; fmtIndex++)
	{
		if (DataFormatNames[fmtIndex].format == format)
		{
			name = DataFormatNames[fmtIndex].name;
			break;
		}
	}

	return name;
}


/*
 * CopyDataCompressionToName returns the name of a copy data compression.
 *
 * We could be clever here and use the enum as an index, but the
 * arrays are small and looping is a bit more robust to future
 * changes.
 */
const char *
CopyDataCompressionToName(CopyDataCompression compression)
{
	const char *name = NULL;

	for (int cmpIndex = 0; DataCompressionNames[cmpIndex].name != NULL; cmpIndex++)
	{
		if (DataCompressionNames[cmpIndex].compression == compression)
		{
			name = DataCompressionNames[cmpIndex].name;
			break;
		}
	}

	return name;
}


/*
 * NameToCopyDataFormat finds a CopyDataFormat by name.
 */
CopyDataFormat
NameToCopyDataFormat(char *formatName)
{
	CopyDataFormat format = DATA_FORMAT_INVALID;

	for (int fmtIndex = 0; DataFormatNames[fmtIndex].name != NULL; fmtIndex++)
	{
		if (strcasecmp(formatName, DataFormatNames[fmtIndex].name) == 0)
		{
			format = DataFormatNames[fmtIndex].format;
			break;
		}
	}

	return format;
}


/*
 * NameToCopyDataCompression finds a CopyDataCompression by name.
 */
CopyDataCompression
NameToCopyDataCompression(char *compressionName)
{
	CopyDataCompression compression = DATA_COMPRESSION_INVALID;

	for (int cmpIndex = 0; DataCompressionNames[cmpIndex].name != NULL; cmpIndex++)
	{
		if (strcasecmp(compressionName, DataCompressionNames[cmpIndex].name) == 0)
		{
			compression = DataCompressionNames[cmpIndex].compression;
			break;
		}
	}

	return compression;
}


/*
 * URLToCopyDataFormat finds a CopyDataFormat based on the URL suffix.
 */
CopyDataFormat
URLToCopyDataFormat(char *url)
{
	CopyDataFormat format = DATA_FORMAT_INVALID;
	char	   *baseURL = GetBaseURL(url);

	for (int extIndex = 0; DataFormatExtensions[extIndex].extension != NULL; extIndex++)
	{
		if (pg_str_endswith(baseURL, DataFormatExtensions[extIndex].extension))
		{
			format = DataFormatExtensions[extIndex].format;
			break;
		}
	}

	return format;
}


/*
 * URLToCopyDataCompression finds a CopyDataCompression based on the URL suffix.
 */
CopyDataCompression
URLToCopyDataCompression(char *url)
{
	CopyDataCompression compression = DATA_COMPRESSION_INVALID;
	char	   *baseURL = GetBaseURL(url);

	for (int extIndex = 0; DataFormatExtensions[extIndex].extension != NULL; extIndex++)
	{
		if (pg_str_endswith(baseURL, DataFormatExtensions[extIndex].extension))
		{
			compression = DataFormatExtensions[extIndex].compression;
			break;
		}
	}

	return compression;
}


/*
 * GetBaseURL returns the part of the URL before the ? or the
 * whole URL if no ? can be found.
 */
char *
GetBaseURL(char *url)
{
	char	   *questionMarkPointer = strchr(url, '?');

	if (questionMarkPointer == NULL)
	{
		return url;
	}

	/* allocate a new string containing only the part before ? */
	int			baseLength = questionMarkPointer - url;
	char	   *baseURL = palloc(baseLength + 1);

	strncpy(baseURL, url, baseLength);
	baseURL[baseLength] = '\0';

	return baseURL;
}


/*
 * OptionsToCopyDataFormat returns the CopyDataFormat specified
 * by a format option in the options list, if any, and if recognized.
 */
CopyDataFormat
OptionsToCopyDataFormat(List *copyOptions)
{
	CopyDataFormat format = DATA_FORMAT_INVALID;

	ListCell   *optionCell = NULL;

	foreach(optionCell, copyOptions)
	{
		DefElem    *option = lfirst(optionCell);

		if (strcmp(option->defname, "format") == 0)
		{
			char	   *formatName = defGetString(option);

			format = NameToCopyDataFormat(formatName);
			break;
		}
	}

	return format;
}


/*
 * OptionsToCopyDataCompression returns the CopyDataCompression specified
 * by a compression option in the options list, if any, and if recognized.
 */
CopyDataCompression
OptionsToCopyDataCompression(List *copyOptions)
{
	CopyDataCompression compression = DATA_COMPRESSION_INVALID;

	ListCell   *optionCell = NULL;

	foreach(optionCell, copyOptions)
	{
		DefElem    *option = lfirst(optionCell);

		if (strcmp(option->defname, "compression") == 0)
		{
			char	   *compressionName = defGetString(option);

			compression = NameToCopyDataCompression(compressionName);
			break;
		}
	}

	return compression;
}


/*
 * URLToCopyDataFormat finds a CopyDataFormat based on the URL suffix.
 */
const char *
FormatToFileExtension(CopyDataFormat format, CopyDataCompression compression)
{
	/*
	 * Parquet files use a single extension (Iceberg data files are also
	 * Parquet)
	 */
	if (FormatUsesParquet(format))
		return ".parquet";

	/* CSV/JSON extension depends on compression */
	for (int extIndex = 0; DataFormatExtensions[extIndex].extension != NULL; extIndex++)
	{
		if (DataFormatExtensions[extIndex].format == format &&
			DataFormatExtensions[extIndex].compression == compression)
		{
			return DataFormatExtensions[extIndex].extension;
		}
	}

	return NULL;
}


/*
 * IsSupportedURL returns whether the given path is prefixed with
 * one of the supported URL protocols.
 */
bool
IsSupportedURL(const char *path)
{
	if (path == NULL)
	{
		return false;
	}

	/*
	 * We added (public) saved query prefix here ahead of more proper HTTPS
	 * support as a small easter egg and because it seems nice for the overall
	 * user experience.
	 */

	return strncmp(path, S3_URL_PREFIX, strlen(S3_URL_PREFIX)) == 0 ||
		strncmp(path, GCS_URL_PREFIX, strlen(GCS_URL_PREFIX)) == 0 ||
		strncmp(path, AZURE_URL_PREFIX, strlen(AZURE_URL_PREFIX)) == 0 ||
		strncmp(path, AZURE_BLOB_URL_PREFIX, strlen(AZURE_BLOB_URL_PREFIX)) == 0 ||
		strncmp(path, AZURE_DLS_URL_PREFIX, strlen(AZURE_DLS_URL_PREFIX)) == 0 ||
		strncmp(path, HTTP_URL_PREFIX, strlen(HTTP_URL_PREFIX)) == 0 ||
		strncmp(path, HTTPS_URL_PREFIX, strlen(HTTPS_URL_PREFIX)) == 0 ||
		strncmp(path, HUGGING_FACE_URL_PREFIX, strlen(HUGGING_FACE_URL_PREFIX)) == 0;
}


/*
 * LowerCaseInPlace ASCII-lowercases a string in place. Hostnames are
 * case-insensitive, so we normalize before suffix matching to avoid trivial
 * bypasses such as "STORAGE.YANDEXCLOUD.NET".
 */
static void
LowerCaseInPlace(char *str)
{
	for (; *str != '\0'; str++)
	{
		if (*str >= 'A' && *str <= 'Z')
			*str += 'a' - 'A';
	}
}


/*
 * ErrorIfDisallowedEndpoint enforces the pg_lake.allowed_endpoint_suffixes
 * GUC. When that GUC is non-empty, http://, https:// and hf:// URLs are only
 * accepted if their host ends with one of the configured suffixes (matched on
 * a dot boundary). This is the main lever a managed service has to prevent
 * server-side request forgery to arbitrary hosts through pgduck_server's
 * httpfs.
 *
 * s3://, gs:// and az:// URLs are intentionally not restricted here: their
 * effective network endpoint is set by the pgduck_server secret, which is not
 * user-controllable, so the bucket/container name in the URL is not a host.
 */
void
ErrorIfDisallowedEndpoint(const char *path)
{
	const char *host;
	const char *hostEnd;
	const char *p;
	const char *at = NULL;
	char	   *hostname;
	char	   *list;
	char	   *token;
	bool		allowed = false;

	if (PgLakeAllowedEndpointSuffixes == NULL ||
		PgLakeAllowedEndpointSuffixes[0] == '\0')
		return;

	if (path == NULL)
		return;

	if (strncmp(path, HUGGING_FACE_URL_PREFIX, strlen(HUGGING_FACE_URL_PREFIX)) == 0)
	{
		/*
		 * hf:// URLs do not carry a host; DuckDB's huggingface extension
		 * always fetches from huggingface.co, so match against that.
		 */
		hostname = pstrdup("huggingface.co");
	}
	else
	{
		if (strncmp(path, HTTP_URL_PREFIX, strlen(HTTP_URL_PREFIX)) == 0)
			host = path + strlen(HTTP_URL_PREFIX);
		else if (strncmp(path, HTTPS_URL_PREFIX, strlen(HTTPS_URL_PREFIX)) == 0)
			host = path + strlen(HTTPS_URL_PREFIX);
		else
			return;

		/* the authority component ends at the first '/', '?' or '#' */
		hostEnd = host + strcspn(host, "/?#");

		/* drop optional userinfo (up to the last '@' in the authority) */
		for (p = host; p < hostEnd; p++)
		{
			if (*p == '@')
				at = p;
		}
		if (at != NULL)
			host = at + 1;

		/* drop optional ':port' */
		for (p = host; p < hostEnd; p++)
		{
			if (*p == ':')
			{
				hostEnd = p;
				break;
			}
		}

		if (hostEnd <= host)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("could not determine host from URL \"%s\"", path)));

		hostname = pnstrdup(host, hostEnd - host);
	}

	LowerCaseInPlace(hostname);

	list = pstrdup(PgLakeAllowedEndpointSuffixes);
	token = list;
	while (token != NULL && *token != '\0')
	{
		char	   *comma = strchr(token, ',');
		char	   *suffix = token;
		size_t		slen;

		if (comma != NULL)
		{
			*comma = '\0';
			token = comma + 1;
		}
		else
			token = NULL;

		/* trim surrounding whitespace */
		while (*suffix == ' ' || *suffix == '\t')
			suffix++;
		slen = strlen(suffix);
		while (slen > 0 && (suffix[slen - 1] == ' ' || suffix[slen - 1] == '\t'))
			suffix[--slen] = '\0';

		if (slen == 0)
			continue;

		LowerCaseInPlace(suffix);

		if (pg_str_endswith(hostname, suffix))
		{
			size_t		hlen = strlen(hostname);

			/* require a full match or a dot boundary before the suffix */
			if (hlen == slen || hostname[hlen - slen - 1] == '.')
			{
				allowed = true;
				break;
			}
		}
	}

	if (!allowed)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("endpoint host \"%s\" is not allowed", hostname),
				 errdetail("pg_lake.allowed_endpoint_suffixes restricts http://, "
						   "https:// and hf:// URLs to the configured hostname suffixes.")));
}


/*
 * GetPgLakeStageLocation returns the base URL configured for @STAGE/ resolution,
 * with trailing slash removed if present.
 */
char *
GetPgLakeStageLocation(void)
{
	bool		inPlace = false;

	return StripTrailingSlash(PgLakeStageLocation, inPlace);
}


/*
 * ResolveStageURL resolves @STAGE/ prefix in paths to the configured
 * stage location. Returns the original path if it doesn't start with @STAGE/,
 * or returns a resolved URL if it does.
 */
char *
ResolveStageURL(const char *path)
{
	if (path == NULL)
	{
		return NULL;
	}

	/* Check for @STAGE/ prefix (case-insensitive) */
	size_t		prefixLen = strlen(STAGE_URL_PREFIX);

	if (pg_strncasecmp(path, STAGE_URL_PREFIX, prefixLen) != 0)
	{
		/* Not a stage URL, return as-is */
		return (char *) path;
	}

	/* Get the configured stage location */
	char	   *baseUrl = GetPgLakeStageLocation();

	if (baseUrl == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("@STAGE/ URL prefix used but pg_lake.stage_location is not configured"),
				 errhint("Set pg_lake.stage_location to your bucket URL (e.g., SET pg_lake.stage_location TO 's3://my-bucket/prefix')")));
	}

	/* Extract the path after @STAGE/ */
	const char *relativePath = path + prefixLen;

	/* Concatenate base URL + "/" + relative path */
	StringInfoData resolvedUrl;

	initStringInfo(&resolvedUrl);
	appendStringInfo(&resolvedUrl, "%s/%s", baseUrl, relativePath);

	return resolvedUrl.data;
}


/*
 * FindDataFormatAndCompression tries to determine the format and compression
 * from the URL and copy options, or throws an error if they cannot be
 * determined.
 */
void
FindDataFormatAndCompression(PgLakeTableType tableType,
							 char *path, List *copyOptions,
							 CopyDataFormat * format,
							 CopyDataCompression * compression)
{
	*format = DATA_FORMAT_INVALID;
	*compression = DATA_COMPRESSION_INVALID;

	/*
	 * First, guess the format and compression from the path.
	 *
	 * We treat the path as a URL, which mainly means that if it contains a ?
	 * symbol we check the suffix before the ? symbol.
	 */
	if (path != NULL)
	{
		*format = URLToCopyDataFormat(path);
		*compression = URLToCopyDataCompression(path);
	}

	/* override the format/compression if explicitly specified */
	ListCell   *optionCell = NULL;

	foreach(optionCell, copyOptions)
	{
		DefElem    *option = lfirst(optionCell);

		if (strcmp(option->defname, "format") == 0)
		{
			char	   *formatName = defGetString(option);

			*format = NameToCopyDataFormat(formatName);
			if (*format == DATA_FORMAT_INVALID)
			{
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("pg_lake_copy: format \"%s\" not recognized",
									   formatName)));
			}
		}
		else if (strcmp(option->defname, "compression") == 0)
		{
			char	   *compressionName = defGetString(option);

			*compression = NameToCopyDataCompression(compressionName);
			if (*compression == DATA_COMPRESSION_INVALID)
			{
				ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("pg_lake_copy: compression \"%s\" not recognized",
									   compressionName)));
			}
		}
	}

	if (tableType == PG_LAKE_ICEBERG_TABLE_TYPE)
	{
		*format = DATA_FORMAT_ICEBERG;
		*compression = DATA_COMPRESSION_SNAPPY;
	}

	if (*format == DATA_FORMAT_INVALID)
	{
		/* no explicit format, and extension not recognized */
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("pg_lake_copy: unrecognized file format")));
	}

	if (*compression == DATA_COMPRESSION_INVALID)
	{
		if (FormatUsesParquet(*format))
		{
			/*
			 * For Parquet we use snappy if not specified because it appears
			 * to be the most common compression algorithm for Parquet. This
			 * value is not used for reads, since the metadata of Parquet
			 * files specifies which compression algorithm is used.
			 */
			*compression = DATA_COMPRESSION_SNAPPY;
		}
		else
		{
			/*
			 * If we do not recognize the extension, do have an explicit
			 * format, but do not have an explicit compression, then default
			 * to 'none'
			 */
			*compression = DATA_COMPRESSION_NONE;
		}
	}

	if (*format == DATA_FORMAT_GDAL)
	{
		if (*compression != DATA_COMPRESSION_NONE &&
			*compression != DATA_COMPRESSION_GZIP &&
			*compression != DATA_COMPRESSION_ZIP)
		{
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("%s compression is not supported for GDAL",
								   CopyDataCompressionToName(*compression))));

		}
	}
}
