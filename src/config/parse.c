/***********************************************************************************************************************************
Command and Option Parse
***********************************************************************************************************************************/
#include "build.auto.h"

#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/error.h"
#include "common/ini.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/regExp.h"
#include "config/config.intern.h"
#include "config/define.h"
#include "config/parse.h"
#include "storage/helper.h"
#include "version.h"

/***********************************************************************************************************************************
Standard config file name and old default path and name
***********************************************************************************************************************************/
#define PGBACKREST_CONFIG_FILE                                      PROJECT_BIN ".conf"
#define PGBACKREST_CONFIG_ORIG_PATH_FILE                            "/etc/" PGBACKREST_CONFIG_FILE
    STRING_STATIC(PGBACKREST_CONFIG_ORIG_PATH_FILE_STR,             PGBACKREST_CONFIG_ORIG_PATH_FILE);

/***********************************************************************************************************************************
Prefix for environment variables
***********************************************************************************************************************************/
#define PGBACKREST_ENV                                              "PGBACKREST_"
#define PGBACKREST_ENV_SIZE                                         (sizeof(PGBACKREST_ENV) - 1)

// In some environments this will not be extern'd
extern char **environ;

/***********************************************************************************************************************************
Standard config include path name
***********************************************************************************************************************************/
#define PGBACKREST_CONFIG_INCLUDE_PATH                              "conf.d"

/***********************************************************************************************************************************
Option value constants
***********************************************************************************************************************************/
VARIANT_STRDEF_STATIC(OPTION_VALUE_0,                               "0");
VARIANT_STRDEF_STATIC(OPTION_VALUE_1,                               "1");

/***********************************************************************************************************************************
Parse option flags
***********************************************************************************************************************************/
// Offset the option values so they don't conflict with getopt_long return codes
#define PARSE_OPTION_FLAG                                           (1 << 30)

// Add a flag for negation rather than checking "--no-"
#define PARSE_NEGATE_FLAG                                           (1 << 29)

// Add a flag for reset rather than checking "--reset-"
#define PARSE_RESET_FLAG                                            (1 << 28)

// Indicate that option name has been deprecated and will be removed in a future release
#define PARSE_DEPRECATE_FLAG                                        (1 << 27)

// Mask for option id
#define PARSE_OPTION_MASK                                           0xFF

// Shift and mask for option index
#define PARSE_INDEX_SHIFT                                           8
#define PARSE_INDEX_MASK                                            0xFF

/***********************************************************************************************************************************
Include automatically generated data structure for getopt_long()
***********************************************************************************************************************************/
#include "config/parse.auto.c"

/***********************************************************************************************************************************
Struct to hold options parsed from the command line
***********************************************************************************************************************************/
typedef struct ParseOptionValue
{
    bool found:1;                                                   // Was the option found on the command line?
    bool negate:1;                                                  // Was the option negated on the command line?
    bool reset:1;                                                   // Was the option reset on the command line?
    unsigned int source:2;                                          // Where was to option found?
    StringList *valueList;                                          // List of values found
} ParseOptionValue;

typedef struct ParseOption
{
    unsigned int indexListAlloc;                                    // Allocated size of index list
    unsigned int indexListTotal;                                    // Total options in indexed list
    ParseOptionValue *indexList;                                    // List of indexed option values
} ParseOption;

#define FUNCTION_LOG_PARSE_OPTION_FORMAT(value, buffer, bufferSize)                                                                \
    typeToLog("ParseOption", buffer, bufferSize)

/***********************************************************************************************************************************
Get the indexed value, creating the array to contain it if needed
***********************************************************************************************************************************/
static ParseOptionValue *
parseOptionIdxValue(ParseOption *option, unsigned int optionIdx)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(PARSE_OPTION, parseOption);
        FUNCTION_TEST_PARAM(UINT, optionIdx);
    FUNCTION_TEST_END();

    if (optionIdx < option->indexListTotal)
        FUNCTION_TEST_RETURN(&option->indexList[optionIdx]);

    FUNCTION_TEST_RETURN(NULL);
}

/***********************************************************************************************************************************
Find an option by name in the option list
***********************************************************************************************************************************/
static unsigned int
optionFind(const String *option)
{
    unsigned int findIdx = 0;

    while (optionList[findIdx].name != NULL)
    {
        if (strcmp(strZ(option), optionList[findIdx].name) == 0)
            break;

        findIdx++;
    }

    return findIdx;
}

/***********************************************************************************************************************************
Convert the value passed into bytes and update valueDbl for range checking
***********************************************************************************************************************************/
static double
sizeQualifierToMultiplier(char qualifier)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(CHAR, qualifier);
    FUNCTION_TEST_END();

    double result;

    switch (qualifier)
    {
        case 'b':
        {
            result = 1;
            break;
        }

        case 'k':
        {
            result = 1024;
            break;
        }

        case 'm':
        {
            result = 1024 * 1024;
            break;
        }

        case 'g':
        {
            result = 1024 * 1024 * 1024;
            break;
        }

        case 't':
        {
            result = 1024LL * 1024LL * 1024LL * 1024LL;
            break;
        }

        case 'p':
        {
            result = 1024LL * 1024LL * 1024LL * 1024LL * 1024LL;
            break;
        }

        default:
            THROW_FMT(AssertError, "'%c' is not a valid size qualifier", qualifier);
    }

    FUNCTION_TEST_RETURN(result);
}

static void
convertToByte(String **value, double *valueDbl)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(STRING, value);
        FUNCTION_LOG_PARAM_P(DOUBLE, valueDbl);
    FUNCTION_LOG_END();

    ASSERT(valueDbl != NULL);

    // Make a copy of the value so it is not updated until we know the conversion will succeed
    String *result = strLower(strDup(*value));

    // Match the value against possible values
    if (regExpMatchOne(STRDEF("^[0-9]+(kb|k|mb|m|gb|g|tb|t|pb|p|b)*$"), result))
    {
        // Get the character array and size
        const char *strArray = strZ(result);
        size_t size = strSize(result);
        int chrPos = -1;

        // If there is a 'b' on the end, then see if the previous character is a number
        if (strArray[size - 1] == 'b')
        {
            // If the previous character is a number, then the letter to look at is 'b' which is the last position else it is in the
            // next to last position (e.g. kb - so the 'k' is the position of interest).  Only need to test for <= 9 since the regex
            // enforces the format.
            if (strArray[size - 2] <= '9')
                chrPos = (int)(size - 1);
            else
                chrPos = (int)(size - 2);
        }
        // else if there is no 'b' at the end but the last position is not a number then it must be one of the letters, e.g. 'k'
        else if (strArray[size - 1] > '9')
            chrPos = (int)(size - 1);

        double multiplier = 1;

        // If a letter was found calculate multiplier, else do nothing since assumed value is already in bytes
        if (chrPos != -1)
        {
            multiplier = sizeQualifierToMultiplier(strArray[chrPos]);

            // Remove any letters
            strTrunc(result, chrPos);
        }

        // Convert string to bytes
        double newDbl = varDblForce(VARSTR(result)) * multiplier;
        result = varStrForce(VARDBL(newDbl));

        // If nothing has blown up then safe to overwrite the original values
        *valueDbl = newDbl;
        *value = result;
    }
    else
        THROW_FMT(FormatError, "value '%s' is not valid", strZ(*value));

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Load the configuration file(s)

The parent mem context is used. Defaults are passed to make testing easier.

Rules:
- config and config-include-path are default. In this case, the config file will be loaded, if it exists, and *.conf files in the
  config-include-path will be appended, if they exist. A missing/empty dir will be ignored except that the original default
  for the config file will be attempted to be loaded if the current default is not found.
- config only is specified. Only the specified config file will be loaded and is required. The default config-include-path will be
  ignored.
- config and config-path are specified. The specified config file will be loaded and is required. The overridden default of the
  config-include-path (<config-path>/conf.d) will be loaded if exists but is not required.
- config-include-path only is specified. *.conf files in the config-include-path will be loaded and the path is required to exist.
  The default config will be be loaded if it exists.
- config-include-path and config-path are specified. The *.conf files in the config-include-path will be loaded and the directory
  passed must exist. The overridden default of the config file path (<config-path>/pgbackrest.conf) will be loaded if exists but is
  not required.
- If the config and config-include-path are specified. The config file will be loaded and is expected to exist and *.conf files in
  the config-include-path will be appended and at least one is expected to exist.
- If --no-config is specified and --config-include-path is specified then only *.conf files in the config-include-path will be
  loaded; the directory is required.
- If --no-config is specified and --config-path is specified then only *.conf files in the overridden default config-include-path
  (<config-path>/conf.d) will be loaded if exist but not required.
- If --no-config is specified and neither --config-include-path nor --config-path are specified then no configs will be loaded.
- If --config-path only, the defaults for config and config-include-path will be changed to use that as a base path but the files
  will not be required to exist since this is a default override.
***********************************************************************************************************************************/
static void
cfgFileLoadPart(String **config, const Buffer *configPart)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(STRING, config);
        FUNCTION_LOG_PARAM(BUFFER, configPart);
    FUNCTION_LOG_END();

    if (configPart != NULL)
    {
        String *configPartStr = strNewBuf(configPart);

        // Validate the file by parsing it as an Ini object. If the file is not properly formed, an error will occur.
        if (strSize(configPartStr) > 0)
        {
            Ini *configPartIni = iniNew();
            iniParse(configPartIni, configPartStr);

            // Create the result config file
            if (*config == NULL)
                *config = strNew("");
            // Else add an LF in case the previous file did not end with one
            else

            // Add the config part to the result config file
            strCat(*config, LF_STR);
            strCat(*config, configPartStr);
        }
    }

    FUNCTION_LOG_RETURN_VOID();
}

static String *
cfgFileLoad(                                                        // NOTE: Passing defaults to enable more complete test coverage
    const ParseOption *optionList,                                  // All options and their current settings
    const String *optConfigDefault,                                 // Current default for --config option
    const String *optConfigIncludePathDefault,                      // Current default for --config-include-path option
    const String *origConfigDefault)                                // Original --config option default (/etc/pgbackrest.conf)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM_P(PARSE_OPTION, optionList);
        FUNCTION_LOG_PARAM(STRING, optConfigDefault);
        FUNCTION_LOG_PARAM(STRING, optConfigIncludePathDefault);
        FUNCTION_LOG_PARAM(STRING, origConfigDefault);
    FUNCTION_LOG_END();

    ASSERT(optionList != NULL);
    ASSERT(optConfigDefault != NULL);
    ASSERT(optConfigIncludePathDefault != NULL);
    ASSERT(origConfigDefault != NULL);

    bool loadConfig = true;
    bool loadConfigInclude = true;

    // If the option is specified on the command line, then found will be true meaning the file is required to exist,
    // else it is optional
    bool configRequired = optionList[cfgOptConfig].indexList != NULL && optionList[cfgOptConfig].indexList[0].found;
    bool configPathRequired = optionList[cfgOptConfigPath].indexList != NULL && optionList[cfgOptConfigPath].indexList[0].found;
    bool configIncludeRequired =
        optionList[cfgOptConfigIncludePath].indexList != NULL && optionList[cfgOptConfigIncludePath].indexList[0].found;

    // Save default for later determining if must check old original default config path
    const String *optConfigDefaultCurrent = optConfigDefault;

    // If the config-path option is found on the command line, then its value will override the base path defaults for config and
    // config-include-path
    if (configPathRequired)
    {
        optConfigDefault = strNewFmt(
            "%s/%s", strZ(strLstGet(optionList[cfgOptConfigPath].indexList[0].valueList, 0)), strBaseZ(optConfigDefault));
        optConfigIncludePathDefault = strNewFmt(
            "%s/%s", strZ(strLstGet(optionList[cfgOptConfigPath].indexList[0].valueList, 0)), PGBACKREST_CONFIG_INCLUDE_PATH);
    }

    // If the --no-config option was passed then do not load the config file
    if (optionList[cfgOptConfig].indexList != NULL && optionList[cfgOptConfig].indexList[0].negate)
    {
        loadConfig = false;
        configRequired = false;
    }

    // If --config option is specified on the command line but neither the --config-include-path nor the config-path are passed,
    // then do not attempt to load the include files
    if (configRequired && !(configPathRequired || configIncludeRequired))
    {
        loadConfigInclude = false;
        configIncludeRequired = false;
    }

    String *result = NULL;

    // Load the main config file
    if (loadConfig)
    {
        const String *configFileName = NULL;

        // Get the config file name from the command-line if it exists else default
        if (configRequired)
            configFileName = strLstGet(optionList[cfgOptConfig].indexList[0].valueList, 0);
        else
            configFileName = optConfigDefault;

        // Load the config file
        Buffer *buffer = storageGetP(storageNewReadP(storageLocal(), configFileName, .ignoreMissing = !configRequired));

        // Convert the contents of the file buffer to the config string object
        if (buffer != NULL)
            result = strNewBuf(buffer);
        else if (strEq(configFileName, optConfigDefaultCurrent))
        {
            // If confg is current default and it was not found, attempt to load the config file from the old default location
            buffer = storageGetP(storageNewReadP(storageLocal(), origConfigDefault, .ignoreMissing = !configRequired));

            if (buffer != NULL)
                result = strNewBuf(buffer);
        }
    }

    // Load *.conf files from the include directory
    if (loadConfigInclude)
    {
        if (result != NULL)
        {
            // Validate the file by parsing it as an Ini object. If the file is not properly formed, an error will occur.
            Ini *ini = iniNew();
            iniParse(ini, result);
        }

        const String *configIncludePath = NULL;

        // Get the config include path from the command-line if it exists else default
        if (configIncludeRequired)
            configIncludePath = strLstGet(optionList[cfgOptConfigIncludePath].indexList[0].valueList, 0);
        else
            configIncludePath = optConfigIncludePathDefault;

        // Get a list of conf files from the specified path -error on missing directory if the option was passed on the command line
        StringList *list = storageListP(
            storageLocal(), configIncludePath, .expression = STRDEF(".+\\.conf$"), .errorOnMissing = configIncludeRequired,
            .nullOnMissing = !configIncludeRequired);

        // If conf files are found, then add them to the config string
        if (list != NULL && strLstSize(list) > 0)
        {
            // Sort the list for reproducibility only -- order does not matter
            strLstSort(list, sortOrderAsc);

            for (unsigned int listIdx = 0; listIdx < strLstSize(list); listIdx++)
            {
                cfgFileLoadPart(
                    &result,
                    storageGetP(
                        storageNewReadP(
                            storageLocal(), strNewFmt("%s/%s", strZ(configIncludePath), strZ(strLstGet(list, listIdx))),
                            .ignoreMissing = true)));
            }
        }
    }

    FUNCTION_LOG_RETURN(STRING, result);
}

/***********************************************************************************************************************************
??? Add validation of section names and check all sections for invalid options in the check command.  It's too expensive to add the
logic to this critical path code.
***********************************************************************************************************************************/
void
configParse(unsigned int argListSize, const char *argList[], bool resetLogLevel)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(UINT, argListSize);
        FUNCTION_LOG_PARAM(CHARPY, argList);
    FUNCTION_LOG_END();

    // Initialize configuration
    cfgInit();

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Set the exe
        cfgExeSet(STR(argList[0]));

        // Phase 1: parse command line parameters
        // -------------------------------------------------------------------------------------------------------------------------
        int option;                                                     // Code returned by getopt_long
        int optionListIdx;                                              // Index of option is list (if an option was returned)
        bool argFound = false;                                          // Track args found to decide on error or help at the end
        StringList *commandParamList = NULL;                            // List of command  parameters

        // Reset optind to 1 in case getopt_long has been called before
        optind = 1;

        // Don't error automatically on unknown options - they will be processed in the loop below
        opterr = false;

        // List of parsed options
        ParseOption parseOptionList[CFG_OPTION_TOTAL] = {0};

        // Only the first non-option parameter should be treated as a command so track if the command has been set
        bool commandSet = false;

        while ((option = getopt_long((int)argListSize, (char **)argList, "-:", optionList, &optionListIdx)) != -1)
        {
            switch (option)
            {
                // Parse arguments that are not options, i.e. commands and parameters passed to commands
                case 1:
                {
                    // The first argument should be the command
                    if (!commandSet)
                    {
                        const char *command = argList[optind - 1];

                        // Try getting the command from the valid command list
                        ConfigCommand commandId = cfgCommandId(command, false);
                        ConfigCommandRole commandRoleId = cfgCmdRoleDefault;

                        // If not successful then a command role may be appended
                        if (commandId == cfgCmdNone)
                        {
                            const StringList *commandPart = strLstNewSplit(STR(command), COLON_STR);

                            if (strLstSize(commandPart) == 2)
                            {
                                // Get command id
                                commandId = cfgCommandId(strZ(strLstGet(commandPart, 0)), false);

                                // If command id is valid then get command role id
                                if (commandId != cfgCmdNone)
                                    commandRoleId = cfgCommandRoleEnum(strLstGet(commandPart, 1));
                            }
                        }

                        // Error when command does not exist
                        if (commandId == cfgCmdNone)
                            THROW_FMT(CommandInvalidError, "invalid command '%s'", command);

                        //  Set the command
                        cfgCommandSet(commandId, commandRoleId);

                        if (cfgCommand() == cfgCmdHelp)
                            cfgCommandHelpSet(true);
                        else
                            commandSet = true;
                    }
                    // Additional arguments are command arguments
                    else
                    {
                        if (commandParamList == NULL)
                            commandParamList = strLstNew();

                        strLstAdd(commandParamList, STR(argList[optind - 1]));
                    }

                    break;
                }

                // If the option is unknown then error
                case '?':
                    THROW_FMT(OptionInvalidError, "invalid option '%s'", argList[optind - 1]);

                // If the option is missing an argument then error
                case ':':
                    THROW_FMT(OptionInvalidError, "option '%s' requires argument", argList[optind - 1]);

                // Parse valid option
                default:
                {
                    // Get option id and flags from the option code
                    ConfigOption optionId = option & PARSE_OPTION_MASK;
                    unsigned int optionIdx = (option >> PARSE_INDEX_SHIFT) & PARSE_INDEX_MASK;
                    bool negate = option & PARSE_NEGATE_FLAG;
                    bool reset = option & PARSE_RESET_FLAG;

                    // Make sure the option id is valid
                    ASSERT(optionId < CFG_OPTION_TOTAL);

                    // Error if this option is secure and cannot be passed on the command line
                    if (cfgDefOptionSecure(optionId))
                    {
                        THROW_FMT(
                            OptionInvalidError,
                            "option '%s' is not allowed on the command-line\n"
                            "HINT: this option could expose secrets in the process list.\n"
                            "HINT: specify the option in a configuration file or an environment variable instead.",
                            cfgOptionIdxName(optionId, optionIdx));
                    }

                    // If the the option has not been found yet then set it
                    ParseOptionValue *optionValue = parseOptionIdxValue(&parseOptionList[optionId], optionIdx);

                    if (!optionValue->found)
                    {
                        *optionValue = (ParseOptionValue)
                        {
                            .found = true,
                            .negate = negate,
                            .reset = reset,
                            .source = cfgSourceParam,
                        };

                        // Only set the argument if the option requires one
                        if (optionList[optionListIdx].has_arg == required_argument)
                        {
                            optionValue->valueList = strLstNew();
                            strLstAdd(optionValue->valueList, STR(optarg));
                        }
                    }
                    else
                    {
                        // Make sure option is not negated more than once.  It probably wouldn't hurt anything to accept this case
                        // but there's no point in allowing the user to be sloppy.
                        if (optionValue->negate && negate)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' is negated multiple times", cfgOptionIdxName(optionId, optionIdx));
                        }

                        // Make sure option is not reset more than once.  Same justification as negate.
                        if (optionValue->reset && reset)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' is reset multiple times", cfgOptionIdxName(optionId, optionIdx));
                        }

                        // Don't allow an option to be both negated and reset
                        if ((optionValue->reset && negate) || (optionValue->negate && reset))
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be negated and reset",
                                cfgOptionIdxName(optionId, optionIdx));
                        }

                        // Don't allow an option to be both set and negated
                        if (optionValue->negate != negate)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be set and negated", cfgOptionIdxName(optionId, optionIdx));
                        }

                        // Don't allow an option to be both set and reset
                        if (optionValue->reset != reset)
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be set and reset", cfgOptionIdxName(optionId, optionIdx));
                        }

                        // Add the argument
                        if (optionList[optionListIdx].has_arg == required_argument && cfgDefOptionMulti(optionId))
                        {
                            strLstAdd(optionValue->valueList, strNew(optarg));
                        }
                        // Error if the option does not accept multiple arguments
                        else
                        {
                            THROW_FMT(
                                OptionInvalidError, "option '%s' cannot be set multiple times",
                                cfgOptionIdxName(optionId, optionIdx));
                        }
                    }

                    break;
                }
            }

            // Arg has been found
            argFound = true;
        }

        // Handle command not found
        if (!commandSet && !cfgCommandHelp())
        {
            // If there are args then error
            if (argFound)
                THROW_FMT(CommandRequiredError, "no command found");

            // Otherwise set the command to help
            cfgCommandHelpSet(true);
        }

        // Set command params
        if (commandParamList != NULL)
        {
            if (!cfgCommandHelp() && !cfgParameterAllowed())
                THROW(ParamInvalidError, "command does not allow parameters");

            cfgCommandParamSet(commandParamList);
        }

        // Enable logging (except for local and remote commands) so config file warnings will be output
        if (cfgCommandRole() != cfgCmdRoleLocal && cfgCommandRole() != cfgCmdRoleRemote && resetLogLevel)
            logInit(logLevelWarn, logLevelWarn, logLevelOff, false, 0, 1, false);

        // Only continue if command options need to be validated, i.e. a real command is running or we are getting help for a
        // specific command and would like to display actual option values in the help.
        if (cfgCommand() != cfgCmdNone &&
            cfgCommand() != cfgCmdVersion &&
            cfgCommand() != cfgCmdHelp)
        {
            // Phase 2: parse environment variables
            // ---------------------------------------------------------------------------------------------------------------------
            ConfigCommand commandId = cfgCommand();
            unsigned int environIdx = 0;

            // Loop through all environment variables and look for our env vars by matching the prefix
            while (environ[environIdx] != NULL)
            {
                const char *keyValue = environ[environIdx];
                environIdx++;

                if (strstr(keyValue, PGBACKREST_ENV) == keyValue)
                {
                    // Find the first = char
                    const char *equalPtr = strchr(keyValue, '=');
                    ASSERT(equalPtr != NULL);

                    // Get key and value
                    const String *key = strReplaceChr(
                        strLower(strNewN(keyValue + PGBACKREST_ENV_SIZE, (size_t)(equalPtr - (keyValue + PGBACKREST_ENV_SIZE)))),
                        '_', '-');
                    const String *value = STR(equalPtr + 1);

                    // Find the option
                    unsigned int findIdx = optionFind(key);

                    // Warn if the option not found
                    if (optionList[findIdx].name == NULL)
                    {
                        LOG_WARN_FMT("environment contains invalid option '%s'", strZ(key));
                        continue;
                    }
                    // Warn if negate option found in env
                    else if (optionList[findIdx].val & PARSE_NEGATE_FLAG)
                    {
                        LOG_WARN_FMT("environment contains invalid negate option '%s'", strZ(key));
                        continue;
                    }
                    // Warn if reset option found in env
                    else if (optionList[findIdx].val & PARSE_RESET_FLAG)
                    {
                        LOG_WARN_FMT("environment contains invalid reset option '%s'", strZ(key));
                        continue;
                    }

                    ConfigOption optionId = optionList[findIdx].val & PARSE_OPTION_MASK;
                    unsigned int optionIdx = (optionList[findIdx].val >> PARSE_INDEX_SHIFT) & PARSE_INDEX_MASK;

                    // Continue if the option is not valid for this command
                    if (!cfgDefOptionValid(commandId, optionId))
                        continue;

                    if (strSize(value) == 0)
                        THROW_FMT(OptionInvalidValueError, "environment variable '%s' must have a value", strZ(key));

                    // Continue if the option has already been specified on the command line
                    ParseOptionValue *optionValue = parseOptionIdxValue(&parseOptionList[optionId], optionIdx);

                    if (optionValue->found)
                        continue;

                    optionValue->found = true;
                    optionValue->source = cfgSourceConfig;

                    // Convert boolean to string
                    if (cfgDefOptionType(optionId) == cfgDefOptTypeBoolean)
                    {
                        if (strEqZ(value, "n"))
                            optionValue->negate = true;
                        else if (!strEqZ(value, "y"))
                            THROW_FMT(OptionInvalidValueError, "environment boolean option '%s' must be 'y' or 'n'", strZ(key));
                    }
                    // Else split list/hash into separate values
                    else if (cfgDefOptionMulti(optionId))
                    {
                        optionValue->valueList = strLstNewSplitZ(value, ":");
                    }
                    // Else add the string value
                    else
                    {
                        optionValue->valueList = strLstNew();
                        strLstAdd(optionValue->valueList, value);
                    }
                }
            }

            // Phase 3: parse config file unless --no-config passed
            // ---------------------------------------------------------------------------------------------------------------------
            // Load the configuration file(s)
            String *configString = cfgFileLoad(
                parseOptionList, STR(cfgDefOptionDefault(commandId, cfgOptConfig)),
                STR(cfgDefOptionDefault(commandId, cfgOptConfigIncludePath)), PGBACKREST_CONFIG_ORIG_PATH_FILE_STR);

            if (configString != NULL)
            {
                Ini *config = iniNew();
                iniParse(config, configString);
                // Get the stanza name
                String *stanza = NULL;

                if (parseOptionList[cfgOptStanza].indexList != NULL && parseOptionList[cfgOptStanza].indexList[0].found)
                    stanza = strLstGet(parseOptionList[cfgOptStanza].indexList[0].valueList, 0);

                // Build list of sections to search for options
                StringList *sectionList = strLstNew();

                if (stanza != NULL)
                {
                    strLstAdd(sectionList, strNewFmt("%s:%s", strZ(stanza), cfgCommandName(cfgCommand())));
                    strLstAdd(sectionList, stanza);
                }

                strLstAdd(sectionList, strNewFmt(CFGDEF_SECTION_GLOBAL ":%s", cfgCommandName(cfgCommand())));
                strLstAdd(sectionList, CFGDEF_SECTION_GLOBAL_STR);

                // Loop through sections to search for options
                for (unsigned int sectionIdx = 0; sectionIdx < strLstSize(sectionList); sectionIdx++)
                {
                    String *section = strLstGet(sectionList, sectionIdx);
                    StringList *keyList = iniSectionKeyList(config, section);
                    KeyValue *optionFound = kvNew();

                    // Loop through keys to search for options
                    for (unsigned int keyIdx = 0; keyIdx < strLstSize(keyList); keyIdx++)
                    {
                        String *key = strLstGet(keyList, keyIdx);

                        // Find the optionName in the main list
                        unsigned int findIdx = optionFind(key);

                        // Warn if the option not found
                        if (optionList[findIdx].name == NULL)
                        {
                            LOG_WARN_FMT("configuration file contains invalid option '%s'", strZ(key));
                            continue;
                        }
                        // Warn if negate option found in config
                        else if (optionList[findIdx].val & PARSE_NEGATE_FLAG)
                        {
                            LOG_WARN_FMT("configuration file contains negate option '%s'", strZ(key));
                            continue;
                        }
                        // Warn if reset option found in config
                        else if (optionList[findIdx].val & PARSE_RESET_FLAG)
                        {
                            LOG_WARN_FMT("configuration file contains reset option '%s'", strZ(key));
                            continue;
                        }

                        ConfigOption optionId = optionList[findIdx].val & PARSE_OPTION_MASK;
                        unsigned int optionIdx = (optionList[findIdx].val >> PARSE_INDEX_SHIFT) & PARSE_INDEX_MASK;

                        /// Warn if this option should be command-line only
                        if (cfgDefOptionSection(optionId) == cfgDefSectionCommandLine)
                        {
                            LOG_WARN_FMT("configuration file contains command-line only option '%s'", strZ(key));
                            continue;
                        }

                        // Make sure this option does not appear in the same section with an alternate name
                        const Variant *optionFoundKey = VARINT(optionId);
                        const Variant *optionFoundName = kvGet(optionFound, optionFoundKey);

                        if (optionFoundName != NULL)
                        {
                            THROW_FMT(
                                OptionInvalidError, "configuration file contains duplicate options ('%s', '%s') in section '[%s]'",
                                strZ(key), strZ(varStr(optionFoundName)), strZ(section));
                        }
                        else
                            kvPut(optionFound, optionFoundKey, VARSTR(key));

                        // Continue if the option is not valid for this command
                        if (!cfgDefOptionValid(commandId, optionId))
                        {
                            // Warn if it is in a command section
                            if (sectionIdx % 2 == 0)
                            {
                                LOG_WARN_FMT(
                                    "configuration file contains option '%s' invalid for section '%s'", strZ(key),
                                    strZ(section));
                                continue;
                            }

                            continue;
                        }

                        // Continue if stanza option is in a global section
                        if (cfgDefOptionSection(optionId) == cfgDefSectionStanza &&
                            strBeginsWithZ(section, CFGDEF_SECTION_GLOBAL))
                        {
                            LOG_WARN_FMT(
                                "configuration file contains stanza-only option '%s' in global section '%s'", strZ(key),
                                strZ(section));
                            continue;
                        }

                        // Continue if this option has already been found in another section or command-line/environment
                        ParseOptionValue *optionValue = parseOptionIdxValue(&parseOptionList[optionId], optionIdx);

                        if (optionValue->found)
                            continue;

                        optionValue->found = true;
                        optionValue->source = cfgSourceConfig;

                        // Process list
                        if (iniSectionKeyIsList(config, section, key))
                        {
                            // Error if the option cannot be specified multiple times
                            if (!cfgDefOptionMulti(optionId))
                            {
                                THROW_FMT(
                                    OptionInvalidError, "option '%s' cannot be set multiple times",
                                    cfgOptionIdxName(optionId, optionIdx));
                            }

                            optionValue->valueList = iniGetList(config, section, key);
                        }
                        else
                        {
                            // Get the option value
                            const String *value = iniGet(config, section, key);

                            if (strSize(value) == 0)
                            {
                                THROW_FMT(
                                    OptionInvalidValueError, "section '%s', key '%s' must have a value", strZ(section),
                                    strZ(key));
                            }

                            if (cfgDefOptionType(optionId) == cfgDefOptTypeBoolean)
                            {
                                if (strEqZ(value, "n"))
                                    optionValue->negate = true;
                                else if (!strEqZ(value, "y"))
                                    THROW_FMT(OptionInvalidValueError, "boolean option '%s' must be 'y' or 'n'", strZ(key));
                            }
                            // Else add the string value
                            else
                            {
                                optionValue->valueList = strLstNew();
                                strLstAdd(optionValue->valueList, value);
                            }
                        }
                    }
                }
            }

            // Phase 4: create the config and resolve indexed options for each group
            // ---------------------------------------------------------------------------------------------------------------------
            Config *config;

            MEM_CONTEXT_NEW_BEGIN("Config")
            {
                config = memNew(sizeof(Config));

                *config = (Config)
                {
                    .memContext = MEM_CONTEXT_NEW(),
                };
            }
            MEM_CONTEXT_NEW_END();

            // Determine how many indexes are used in each group
            bool groupIdxMap[CFG_OPTION_GROUP_TOTAL][CFG_OPTION_INDEX_MAX] = {0};

            for (unsigned int optionId = 0; optionId < CFG_OPTION_TOTAL; optionId++)
            {
                // Is the option valid for this command?
                if (cfgDefOptionValid(commandId, optionId))
                {
                    config->option[optionId].valid = true;
                }
                else
                {
                    // Error if the invalid option was explicitly set on the command-line
                    if (parseOptionList[optionId].indexListTotal > 0)
                    {
                        THROW_FMT(
                            OptionInvalidError, "option '%s' not valid for command '%s'", cfgOptionName(optionId),
                            cfgCommandName(cfgCommand()));
                    }

                    // Continue to the next option
                    continue;
                }

                if (cfgOptionGroup(optionId))
                {
                    unsigned int groupId = cfgOptionGroupId(optionId);

                    for (unsigned int optionIdx = 0; optionIdx < parseOptionList[optionId].indexListTotal; optionIdx++)
                    {
                        if (parseOptionList[optionId].indexList[optionIdx].found)
                        {
                            if (!groupIdxMap[groupId][optionIdx])
                            {
                                config->optionGroup[groupId].indexTotal++;
                                groupIdxMap[groupId][optionIdx] = true;
                            }
                        }
                    }
                }
            }

            // Write the indexes into the group in order
            for (unsigned int groupId = 0; groupId < CFG_OPTION_GROUP_TOTAL; groupId++)
            {
                unsigned int optionIdxMax = 0;

                for (unsigned int optionIdx = 0; optionIdx < CFG_OPTION_INDEX_MAX; optionIdx++)
                {
                    if (groupIdxMap[groupId][optionIdx])
                    {
                        config->optionGroup[groupId].index[optionIdxMax] = optionIdx;
                        optionIdxMax++;
                    }
                }
            }

            // Phase 5: validate option definitions and load into configuration
            // ---------------------------------------------------------------------------------------------------------------------
            for (unsigned int optionOrderIdx = 0; optionOrderIdx < CFG_OPTION_TOTAL; optionOrderIdx++)
            {
                // Validate options based on the option resolve order.  This allows resolving all options in a single pass.
                ConfigOption optionId = optionResolveOrder[optionOrderIdx];

                // Skip this option if it is not valid
                if (!config->option[optionId].valid)
                    continue;

                // Determine the option index total. For options that are not indexed the index total is 1.
                bool optionGroup = cfgOptionGroup(optionId);
                unsigned int optionGroupId = optionGroup ? cfgOptionGroupId(optionId) : UINT_MAX;
                unsigned int optionIndexTotal = optionGroup ? config->optionGroup[optionGroupId].indexTotal : 1;

                // Loop through the option indexes
                ParseOption *parseOption = &parseOptionList[optionId];
                ConfigDefineOptionType optionDefType = cfgDefOptionType(optionId);

                for (unsigned int optionIdx = 0; optionIdx < optionIndexTotal; optionIdx++)
                {
                    ParseOptionValue *parseOptionValue =
                        parseOptionIdxValue(parseOption, optionGroup ? config->optionGroup[optionGroupId].index[optionIdx] : 0);
                    ConfigOptionValue *configOptionValue = config->option[optionId].index[optionIdx];

                    // Is the value set for this option?
                    bool optionSet =
                        parseOptionValue->found && (optionDefType == cfgDefOptTypeBoolean || !parseOptionValue->negate) &&
                        !parseOptionValue->reset;

                    // Set negate flag
                    configOptionValue->negate = parseOptionValue->negate;

                    // Set reset flag
                    configOptionValue->reset = parseOptionValue->reset;

                    // Check option dependencies
                    bool dependResolved = true;

                    if (cfgDefOptionDepend(commandId, optionId))
                    {
                        ConfigOption dependOptionId = cfgDefOptionDependOption(commandId, optionId);
                        ConfigDefineOptionType dependOptionDefType = cfgDefOptionType(dependOptionId);

                        // Get the depend option value
                        const Variant *dependValue = cfgOption(dependOptionId);

                        if (dependValue != NULL)
                        {
                            if (dependOptionDefType == cfgDefOptTypeBoolean)
                            {
                                if (varBool(cfgOption(dependOptionId)))
                                    dependValue = OPTION_VALUE_1;
                                else
                                    dependValue = OPTION_VALUE_0;
                            }
                        }

                        // Can't resolve if the depend option value is null
                        if (dependValue == NULL)
                        {
                            dependResolved = false;

                            // If depend not resolved and option value is set on the command-line then error.  See unresolved list
                            // depend below for a detailed explanation.
                            if (optionSet && parseOptionValue->source == cfgSourceParam)
                            {
                                THROW_FMT(
                                    OptionInvalidError, "option '%s' not valid without option '%s'", cfgOptionName(optionId),
                                    cfgOptionName(dependOptionId));
                            }
                        }
                        // If a depend list exists, make sure the value is in the list
                        else if (cfgDefOptionDependValueTotal(commandId, optionId) > 0)
                        {
                            dependResolved = cfgDefOptionDependValueValid(commandId, optionId, strZ(varStr(dependValue)));

                            // If depend not resolved and option value is set on the command-line then error.  It's OK to have
                            // unresolved options in the config file because they may be there for another command.  For instance,
                            // spool-path is only loaded for the archive-push command when archive-async=y, and the presence of
                            // spool-path in the config file should not cause an error here, it will just end up null.
                            if (!dependResolved && optionSet && parseOptionValue->source == cfgSourceParam)
                            {
                                // Get the depend option name
                                const String *dependOptionName = STR(cfgOptionName(dependOptionId));

                                // Build the list of possible depend values
                                StringList *dependValueList = strLstNew();

                                for (unsigned int listIdx = 0;
                                        listIdx < cfgDefOptionDependValueTotal(commandId, optionId); listIdx++)
                                {
                                    const char *dependValue = cfgDefOptionDependValue(commandId, optionId, listIdx);

                                    // Build list based on depend option type
                                    if (dependOptionDefType == cfgDefOptTypeBoolean)
                                    {
                                        // Boolean outputs depend option name as no-* when false
                                        if (strcmp(dependValue, "0") == 0)
                                            dependOptionName = strNewFmt("no-%s", cfgOptionName(dependOptionId));
                                    }
                                    else
                                    {
                                        ASSERT(dependOptionDefType == cfgDefOptTypePath || dependOptionDefType == cfgDefOptTypeString);
                                        strLstAdd(dependValueList, strNewFmt("'%s'", dependValue));
                                    }
                                }

                                // Build the error string
                                const String *errorValue = EMPTY_STR;

                                if (strLstSize(dependValueList) == 1)
                                    errorValue = strNewFmt(" = %s", strZ(strLstGet(dependValueList, 0)));
                                else if (strLstSize(dependValueList) > 1)
                                    errorValue = strNewFmt(" in (%s)", strZ(strLstJoin(dependValueList, ", ")));

                                // Throw the error
                                THROW(
                                    OptionInvalidError,
                                    strZ(
                                        strNewFmt(
                                            "option '%s' not valid without option '%s'%s", cfgOptionName(optionId),
                                            strZ(dependOptionName), strZ(errorValue))));
                            }
                        }
                    }

                    // Is the option resolved?
                    if (dependResolved)
                    {
                        // Is the option set?
                        if (optionSet)
                        {
                            if (optionDefType == cfgDefOptTypeBoolean)
                            {
                                cfgOptionSet(optionId, parseOptionValue->source, VARBOOL(!parseOptionValue->negate));
                            }
                            else if (optionDefType == cfgDefOptTypeHash)
                            {
                                Variant *value = varNewKv(kvNew());
                                KeyValue *keyValue = varKv(value);

                                for (unsigned int listIdx = 0; listIdx < strLstSize(parseOptionValue->valueList); listIdx++)
                                {
                                    const char *pair = strZ(strLstGet(parseOptionValue->valueList, listIdx));
                                    const char *equal = strchr(pair, '=');

                                    if (equal == NULL)
                                    {
                                        THROW_FMT(
                                            OptionInvalidError, "key/value '%s' not valid for '%s' option",
                                            strZ(strLstGet(parseOptionValue->valueList, listIdx)), cfgOptionName(optionId));
                                    }

                                    kvPut(keyValue, VARSTR(strNewN(pair, (size_t)(equal - pair))), VARSTRZ(equal + 1));
                                }

                                cfgOptionSet(optionId, parseOptionValue->source, value);
                            }
                            else if (optionDefType == cfgDefOptTypeList)
                            {
                                cfgOptionSet(
                                    optionId, parseOptionValue->source, varNewVarLst(varLstNewStrLst(parseOptionValue->valueList)));
                            }
                            else
                            {
                                String *value = strLstGet(parseOptionValue->valueList, 0);

                                // If a numeric type check that the value is valid
                                if (optionDefType == cfgDefOptTypeInteger || optionDefType == cfgDefOptTypeFloat ||
                                    optionDefType == cfgDefOptTypeSize)
                                {
                                    double valueDbl = 0;

                                    // Check that the value can be converted
                                    TRY_BEGIN()
                                    {
                                        if (optionDefType == cfgDefOptTypeInteger)
                                        {
                                            valueDbl = (double)varInt64Force(VARSTR(value));
                                        }
                                        else if (optionDefType == cfgDefOptTypeSize)
                                        {
                                            convertToByte(&value, &valueDbl);
                                        }
                                        else
                                            valueDbl = varDblForce(VARSTR(value));
                                    }
                                    CATCH_ANY()
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' is not valid for '%s' option", strZ(value),
                                            cfgOptionName(optionId));
                                    }
                                    TRY_END();

                                    // Check value range
                                    if (cfgDefOptionAllowRange(commandId, optionId) &&
                                        (valueDbl < cfgDefOptionAllowRangeMin(commandId, optionId) ||
                                         valueDbl > cfgDefOptionAllowRangeMax(commandId, optionId)))
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' is out of range for '%s' option", strZ(value),
                                            cfgOptionName(optionId));
                                    }
                                }
                                // Else if path make sure it is valid
                                else if (optionDefType == cfgDefOptTypePath)
                                {
                                    // Make sure it is long enough to be a path
                                    if (strSize(value) == 0)
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' must be >= 1 character for '%s' option", strZ(value),
                                            cfgOptionName(optionId));
                                    }

                                    // Make sure it starts with /
                                    if (!strBeginsWithZ(value, "/"))
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' must begin with / for '%s' option", strZ(value),
                                            cfgOptionName(optionId));
                                    }

                                    // Make sure there are no occurrences of //
                                    if (strstr(strZ(value), "//") != NULL)
                                    {
                                        THROW_FMT(
                                            OptionInvalidValueError, "'%s' cannot contain // for '%s' option", strZ(value),
                                            cfgOptionName(optionId));
                                    }

                                    // If the path ends with a / we'll strip it off (unless the value is just /)
                                    if (strEndsWithZ(value, "/") && strSize(value) != 1)
                                        strTrunc(value, (int)strSize(value) - 1);
                                }

                                // If the option has an allow list then check it
                                if (cfgDefOptionAllowList(commandId, optionId) &&
                                    !cfgDefOptionAllowListValueValid(commandId, optionId, strZ(value)))
                                {
                                    THROW_FMT(
                                        OptionInvalidValueError, "'%s' is not allowed for '%s' option", strZ(value),
                                        cfgOptionName(optionId));
                                }

                                cfgOptionSet(optionId, parseOptionValue->source, VARSTR(value));
                            }
                        }
                        else if (parseOptionValue->negate)
                            cfgOptionSet(optionId, parseOptionValue->source, NULL);
                        // Else try to set a default
                        else
                        {
                            // Get the default value for this option
                            const char *value = cfgDefOptionDefault(commandId, optionId);

                            if (value != NULL)
                                cfgOptionSet(optionId, cfgSourceDefault, VARSTRZ(value));
                            else if (cfgDefOptionRequired(commandId, optionId) && !cfgCommandHelp())
                            {
                                const char *hint = "";

                                if (cfgDefOptionSection(optionId) == cfgDefSectionStanza)
                                    hint = "\nHINT: does this stanza exist?";

                                THROW_FMT(
                                    OptionRequiredError, "%s command requires option: %s%s", cfgCommandName(cfgCommand()),
                                    cfgOptionName(optionId), hint);
                            }
                        }
                    }
                }
            }
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
