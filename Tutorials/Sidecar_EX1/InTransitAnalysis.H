#ifndef INTRANSITANALYSIS_H
#define INTRANSITANALYSIS_H

#include <Analysis.H>

class InTransitAnalysis: public AnalysisCallbackInterface
{
    public:
        void Initialize (MultiFab&, Geometry&, int);
        void DoAnalysis ();
        void Finalize ();
        void PrintResults () const;

    private:
        MultiFab* mf;
        Geometry* geom;
        Real norm0;
        Real probsize;
        int time_step;
};

#endif /* INTRANSITANALYSIS_H */
