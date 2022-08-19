/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup collada
 */
#include "ErrorHandler.h"
#include <iostream>

#include "COLLADASaxFWLIError.h"
#include "COLLADASaxFWLSaxFWLError.h"
#include "COLLADASaxFWLSaxParserError.h"

#include "GeneratedSaxParserParserError.h"

#include <cstring>

#include "BLI_utildefines.h"

//--------------------------------------------------------------------
ErrorHandler::ErrorHandler() : mError(false)
{
}

//--------------------------------------------------------------------
bool ErrorHandler::handleError(const COLLADASaxFWL::IError *error)
{
  /* This method must return false when Collada should continue.
   * See https://github.com/KhronosGroup/OpenCOLLADA/issues/442
   */
  bool isError = true;
  std::string error_context;
  std::string error_message;

  if (error->getErrorClass() == COLLADASaxFWL::IError::ERROR_SAXPARSER) {
    error_context = "Schema validation";

    COLLADASaxFWL::SaxParserError *saxParserError = (COLLADASaxFWL::SaxParserError *)error;
    const GeneratedSaxParser::ParserError &parserError = saxParserError->getError();
    error_message = parserError.getErrorMessage();

    if (parserError.getErrorType() ==
        GeneratedSaxParser::ParserError::ERROR_VALIDATION_MIN_OCCURS_UNMATCHED) {
      if (STREQ(parserError.getElement(), "effect")) {
        isError = false;
      }
    }

    else if (parserError.getErrorType() ==
             GeneratedSaxParser::ParserError::
                 ERROR_VALIDATION_SEQUENCE_PREVIOUS_SIBLING_NOT_PRESENT) {
      if (!(STREQ(parserError.getElement(), "extra") &&
            STREQ(parserError.getAdditionalText().c_str(), "sibling: fx_profile_abstract"))) {
        isError = false;
      }
    }

    else if (parserError.getErrorType() ==
             GeneratedSaxParser::ParserError::ERROR_COULD_NOT_OPEN_FILE) {
      isError = true;
      error_context = "File access";
    }

    else if (parserError.getErrorType() ==
             GeneratedSaxParser::ParserError::ERROR_REQUIRED_ATTRIBUTE_MISSING) {
      isError = true;
    }

    else {
      isError = (parserError.getSeverity() !=
                 GeneratedSaxParser::ParserError::Severity::SEVERITY_ERROR_NONCRITICAL);
    }
  }
  else if (error->getErrorClass() == COLLADASaxFWL::IError::ERROR_SAXFWL) {
    error_context = "Sax FWL";
    COLLADASaxFWL::SaxFWLError *saxFWLError = (COLLADASaxFWL::SaxFWLError *)error;
    error_message = saxFWLError->getErrorMessage();

    /*
     * Accept non critical errors as warnings (i.e. texture not found)
     * This makes the importer more graceful, so it now imports what makes sense.
     */

    isError = (saxFWLError->getSeverity() != COLLADASaxFWL::IError::SEVERITY_ERROR_NONCRITICAL);
  }
  else {
    error_context = "OpenCollada";
    error_message = error->getFullErrorMessage();
    isError = true;
  }

  std::string severity = (isError) ? "Error" : "Warning";
  std::cout << error_context << " (" << severity << "): " << error_message << std::endl;
  if (isError) {
    std::cout << "The Collada import has been forced to stop." << std::endl;
    std::cout << "Please fix the reported error and then try again.";
    mError = true;
  }
  return isError;
}
