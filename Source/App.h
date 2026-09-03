#pragma once

#include <wx/app.h>

class wxCmdLineParser;

class PromoApp : public wxApp
{
public:
    bool OnInit() override;

    // wxApp parses the command line before OnInit runs and refuses anything it
    // was not told about — an unknown switch stops the program with "Unknown
    // option" and it never opens. The installer relaunches us with /fromupdate
    // at the end of an automatic update, so that switch has to be declared here
    // or the update ends with the application failing to come back.
    void OnInitCmdLine(wxCmdLineParser& parser) override;
    bool OnCmdLineParsed(wxCmdLineParser& parser) override;

private:
    bool fromUpdate_ = false;
};
